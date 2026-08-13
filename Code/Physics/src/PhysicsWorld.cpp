// Satu-satunya unit terjemahan di `Sim::Physics` yang melihat PhysX.
//
// Batas itu bukan kerapian: ia yang membuat modul ini bisa dibangun tanpa PhysX
// sama sekali, dan yang membuat setiap pemanggil tidak ikut membutuhkan include
// PhysX-nya. Aturan yang sama sudah terbukti tiga kali di pohon ini — backend
// gambar berganti pustaka dua kali tanpa satu pun titik panggil berubah.
//
// Berkasnya tetap ikut dibangun tanpa PhysX dan menolak dengan pesan yang
// menyebut sebabnya, mengikuti pola `UsdImport.cpp`.

#include "Sim/Physics/PhysicsWorld.h"

#include "Sim/Core/Log.h"

#include "VehicleBackend.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

#if SIM_WITH_PHYSX

// PhysX memancarkan peringatannya sendiri; header-nya masuk lewat -isystem di
// CMake, jadi -Werror proyek ini tidak menggagalkan build karenanya.
#include <PxPhysicsAPI.h>

namespace sim::physics {
namespace {

/// Menyalurkan galat PhysX ke log editor.
///
/// **Bukan `PxDefaultErrorCallback`.** Yang bawaan menulis ke stderr, dan pesan
/// yang hilang di stderr tidak pernah sampai ke panel Console — padahal galat
/// PhysX hampir selalu menyebut persis apa yang salah dengan sebuah aktor.
class LogErrorCallback final : public physx::PxErrorCallback {
public:
    void reportError(physx::PxErrorCode::Enum code, const char* message, const char* file,
                     int line) override {
        switch (code) {
            case physx::PxErrorCode::eDEBUG_INFO:
                SIM_INFO("Physics", "{}", message);
                break;
            case physx::PxErrorCode::eDEBUG_WARNING:
            case physx::PxErrorCode::ePERF_WARNING:
                SIM_WARN("Physics", "{}", message);
                break;
            default:
                SIM_ERROR("Physics", "{} ({}:{})", message, file, line);
                break;
        }
    }
};

LogErrorCallback& ErrorCallback() {
    static LogErrorCallback callback;
    return callback;
}

physx::PxVec3 ToPx(const Vec3& v) { return physx::PxVec3(v.x, v.y, v.z); }
Vec3 FromPx(const physx::PxVec3& v) { return Vec3(v.x, v.y, v.z); }
physx::PxQuat ToPx(const Quat& q) { return physx::PxQuat(q.x, q.y, q.z, q.w); }
Quat FromPx(const physx::PxQuat& q) { return Quat(q.w, q.x, q.y, q.z); }

physx::PxTransform ToPx(const Vec3& position, const Quat& rotation) {
    return physx::PxTransform(ToPx(position), ToPx(rotation));
}

}  // namespace

struct PhysicsWorld::Impl {
    physx::PxDefaultAllocator allocator;
    physx::PxFoundation* foundation = nullptr;
    physx::PxPhysics* physics = nullptr;
    physx::PxDefaultCpuDispatcher* dispatcher = nullptr;
    physx::PxScene* scene = nullptr;

    WorldDesc desc;
    std::string error;

    /// Handle → aktor. **Peta, bukan pointer yang dibagikan ke luar**: handle
    /// yang basi dijawab penolakan alih-alih alamat yang kebetulan masih
    /// terbaca.
    std::unordered_map<uint64_t, physx::PxRigidActor*> bodies;
    uint64_t nextHandle = 1;

    /// Aktor → handle. Kebalikan `bodies`, dan ada karena scene query menjawab
    /// dengan `PxRigidActor*`: tanpa peta ini setiap hasil query harus dicari
    /// linear di seluruh benda, yang mengubah raycast murah menjadi mahal justru
    /// pada adegan yang besar.
    std::unordered_map<const physx::PxRigidActor*, uint64_t> handlesByActor;

    /// Handle → sendi.
    ///
    /// **Dilacak terpisah, bukan dibiarkan ikut mati bersama aktornya.** PhysX
    /// menuntut sendi dilepas sebelum benda yang dipegangnya; melepas aktor
    /// lebih dulu meninggalkan sendi yang menunjuk memori yang sudah bebas, dan
    /// itu tidak crash di tempat kejadian melainkan pada langkah simulasi
    /// berikutnya yang kebetulan menyentuhnya.
    std::unordered_map<uint64_t, physx::PxJoint*> joints;
    uint64_t nextJointHandle = 1;

    /// Handle → articulation, dan link-nya menurut urutan `ArticulationDesc`.
    ///
    /// Daftar link disimpan sendiri alih-alih dibaca ulang lewat `getLinks`:
    /// PhysX tidak menjanjikan urutan yang dikembalikannya sama dengan urutan
    /// pembuatan, sedangkan `ReadLinkState` menjanjikan indeks yang sama dengan
    /// yang ditulis pemanggil di `desc.links`.
    /// Kendaraan, beserta konteks simulasi yang dibagi seluruhnya.
    ///
    /// **Konteksnya milik dunia, bukan milik tiap kendaraan**: ia menyebut
    /// gravitasi, kerangka sumbu, dan scene yang sama untuk semuanya, dan
    /// menyalinnya per kendaraan berarti dua puluh salinan yang harus ikut
    /// berubah setiap kali gravitasi disetel.
    std::unordered_map<uint64_t, std::unique_ptr<VehicleInstance>> vehicles;
    uint64_t nextVehicleHandle = 1;
    physx::vehicle2::PxVehiclePhysXSimulationContext vehicleContext;
    physx::PxConvexMesh* unitCylinderMesh = nullptr;
    physx::PxMaterial* vehicleMaterial = nullptr;
    physx::PxCookingParams* cookingParams = nullptr;
    bool vehicleExtensionReady = false;

    std::unordered_map<uint64_t, physx::PxArticulationReducedCoordinate*> articulations;
    std::unordered_map<uint64_t, std::vector<physx::PxArticulationLink*>> articulationLinks;
    uint64_t nextArticulationHandle = 1;

    /// Sedang di dalam `simulate`/`fetchResults`.
    ///
    /// **Atomic karena justru dibaca dari thread lain.** PhysX melarang scene
    /// query berjalan bersamaan dengan langkah simulasi, dan pelanggarannya
    /// tidak crash melainkan mengembalikan keadaan setengah diperbarui — cacat
    /// yang muncul jauh dari sebabnya dan hanya sesekali. Bendera ini yang
    /// mengubahnya menjadi penolakan yang menyebut dirinya.
    std::atomic<bool> stepping{false};

    /// Sisa waktu yang belum cukup untuk satu langkah penuh.
    ///
    /// `double` supaya pembulatan tidak menumpuk di sesi yang panjang; nilainya
    /// dihitung ulang lewat penjumlahan dan pengurangan setiap frame.
    ///
    /// **Yang tidak diperbaikinya perlu diketahui**, karena ia terlihat seperti
    /// bug dan bukan: banyaknya langkah untuk rentang waktu yang sama bisa
    /// berbeda satu, tergantung bagaimana waktu itu dipecah menjadi frame. Satu
    /// detik sebagai 60 frame @16,67 ms memberi 60 langkah; sebagai 20 frame
    /// @50 ms memberi 59. Sebabnya bukan presisi akumulator melainkan
    /// masukannya — `3 × float(1/60)` benar-benar lebih besar daripada
    /// `float(0.05)`, jadi frame 50 ms yang pertama hanya memuat dua langkah
    /// utuh, dan total 20 frame itu memang hanya 59,999998 langkah panjangnya.
    ///
    /// Jadi jaminan langkah tetap adalah **selisih paling banyak satu langkah**,
    /// bukan kesamaan persis. Itu sudah cukup untuk yang dituju: selisihnya
    /// terbatas dan tidak menumpuk, sehingga adegan yang sama tetap mengendap
    /// di keadaan yang sama di mesin yang lebih cepat.
    double accumulator = 0.0;
    uint64_t stepCount = 0;

    ~Impl() { Shutdown(); }

    /// Melepas setiap sendi yang memegang `actor`, dan mengembalikan berapa
    /// banyak. Dipanggil sebelum aktornya dilepas — urutan itu wajib.
    std::size_t ReleaseJointsTouching(const physx::PxRigidActor* actor) {
        std::size_t released = 0;
        for (auto it = joints.begin(); it != joints.end();) {
            physx::PxJoint* joint = it->second;
            if (joint == nullptr) {
                it = joints.erase(it);
                continue;
            }
            physx::PxRigidActor* a = nullptr;
            physx::PxRigidActor* b = nullptr;
            joint->getActors(a, b);
            if (a == actor || b == actor) {
                joint->release();
                it = joints.erase(it);
                ++released;
            } else {
                ++it;
            }
        }
        return released;
    }

    void Shutdown() {
        // **Chassis kendaraan terdaftar dua kali dengan sengaja** — sebagai
        // benda biasa supaya scene query dan sendi bisa menyebutnya, dan sebagai
        // milik kendaraan yang membuatnya. Yang melepasnya hanya boleh satu:
        // `PxVehiclePhysXActorDestroy`. Tanpa pencabutan di bawah ini, gelung
        // `bodies` melepasnya untuk kedua kalinya, dan itu tidak terlihat
        // sebagai galat melainkan sebagai segfault saat dunia ditutup.
        for (auto& [handle, vehicle] : vehicles) {
            if (vehicle == nullptr) {
                continue;
            }
            physx::PxRigidBody* chassis = vehicle->RigidBody();
            if (chassis != nullptr) {
                const auto found = handlesByActor.find(chassis);
                if (found != handlesByActor.end()) {
                    bodies.erase(found->second);
                    handlesByActor.erase(found);
                }
            }
            if (scene != nullptr) {
                vehicle->RemoveFromScene(*scene);
            }
            vehicle->Destroy();
        }
        vehicles.clear();
        if (unitCylinderMesh != nullptr) {
            unitCylinderMesh->release();
            unitCylinderMesh = nullptr;
        }
        if (vehicleMaterial != nullptr) {
            vehicleMaterial->release();
            vehicleMaterial = nullptr;
        }
        delete cookingParams;
        cookingParams = nullptr;
        if (vehicleExtensionReady) {
            physx::vehicle2::PxCloseVehicleExtension();
            vehicleExtensionReady = false;
        }

        // Articulation memiliki link-nya sendiri; melepasnya melepas semuanya.
        for (auto& [handle, articulation] : articulations) {
            if (articulation != nullptr) {
                if (scene != nullptr) {
                    scene->removeArticulation(*articulation);
                }
                articulation->release();
            }
        }
        articulations.clear();
        articulationLinks.clear();

        // Sendi lebih dulu, seluruhnya, sebelum satu aktor pun dilepas.
        for (auto& [handle, joint] : joints) {
            if (joint != nullptr) {
                joint->release();
            }
        }
        joints.clear();

        for (auto& [handle, actor] : bodies) {
            if (actor != nullptr && scene != nullptr) {
                scene->removeActor(*actor);
            }
            if (actor != nullptr) {
                actor->release();
            }
        }
        bodies.clear();

        // Urutannya terbalik dari pembuatannya, dan itu wajib: melepas
        // foundation sebelum scene membuat PhysX membaca alokator yang sudah
        // tidak ada.
        if (scene != nullptr) {
            scene->release();
            scene = nullptr;
        }
        if (dispatcher != nullptr) {
            dispatcher->release();
            dispatcher = nullptr;
        }
        if (physics != nullptr) {
            physics->release();
            physics = nullptr;
        }
        if (foundation != nullptr) {
            foundation->release();
            foundation = nullptr;
        }
    }
};

bool Available() { return true; }

const char* BackendVersion() { return SIM_PHYSX_VERSION_STRING; }

bool GpuAvailable() {
#if SIM_WITH_PHYSX_GPU
    return true;
#else
    return false;
#endif
}

PhysicsWorld::PhysicsWorld() = default;
PhysicsWorld::~PhysicsWorld() = default;
PhysicsWorld::PhysicsWorld(PhysicsWorld&&) noexcept = default;
PhysicsWorld& PhysicsWorld::operator=(PhysicsWorld&&) noexcept = default;

bool PhysicsWorld::Create(const WorldDesc& desc) {
    Destroy();
    auto impl = std::make_unique<Impl>();
    impl->desc = desc;

    if (!(desc.fixedTimeStep > 0.0f)) {
        impl->error = "fixed time step must be positive";
        impl_ = std::move(impl);
        return false;
    }
    if (!(desc.typicalLength > 0.0f) || !(desc.typicalSpeed > 0.0f)) {
        impl->error = "typical length and speed must both be positive";
        impl_ = std::move(impl);
        return false;
    }

    impl->foundation = PxCreateFoundation(PX_PHYSICS_VERSION, impl->allocator, ErrorCallback());
    if (impl->foundation == nullptr) {
        impl->error = "PxCreateFoundation failed";
        impl_ = std::move(impl);
        return false;
    }

    // **Skala disetel sekali, di sini.** Salah menyetelnya tidak muncul sebagai
    // galat melainkan sebagai benda yang bergetar saat diam, atau jatuh seperti
    // di bulan — dan keduanya biasanya "diperbaiki" dengan menyetel massa,
    // yang membuat seluruh adegan salah bersama-sama.
    physx::PxTolerancesScale scale;
    scale.length = desc.typicalLength;
    scale.speed = desc.typicalSpeed;

    impl->physics = PxCreatePhysics(PX_PHYSICS_VERSION, *impl->foundation, scale);
    if (impl->physics == nullptr) {
        impl->error = "PxCreatePhysics failed";
        impl_ = std::move(impl);
        return false;
    }

    physx::PxSceneDesc sceneDesc(impl->physics->getTolerancesScale());
    sceneDesc.gravity = ToPx(desc.gravity);
    impl->dispatcher = physx::PxDefaultCpuDispatcherCreate(desc.workerThreads);
    sceneDesc.cpuDispatcher = impl->dispatcher;
    sceneDesc.filterShader = physx::PxDefaultSimulationFilterShader;
    // **Determinisme diminta secara eksplisit.** Tanpanya PhysX boleh menyusun
    // ulang pekerjaan antar-langkah, dan dua kali jalan dengan masukan yang sama
    // menghasilkan hasil yang sedikit berbeda — yang menghapus seluruh guna uji
    // regresi fisika.
    sceneDesc.flags |= physx::PxSceneFlag::eENABLE_ENHANCED_DETERMINISM;

    impl->scene = impl->physics->createScene(sceneDesc);
    if (impl->scene == nullptr) {
        impl->error = "createScene failed";
        impl_ = std::move(impl);
        return false;
    }

    impl_ = std::move(impl);

    // Ekstensi kendaraan disiapkan bersama dunia, bukan saat kendaraan pertama
    // ditambahkan. Ongkosnya sekali dan kecil, sedangkan menyiapkannya belakangan
    // berarti `AddVehicle` bisa gagal karena alasan yang tidak ada hubungannya
    // dengan kendaraan yang diminta.
    if (physx::vehicle2::PxInitVehicleExtension(*impl_->foundation)) {
        impl_->vehicleExtensionReady = true;
        impl_->cookingParams =
            new physx::PxCookingParams(impl_->physics->getTolerancesScale());
        impl_->vehicleMaterial = impl_->physics->createMaterial(0.8f, 0.8f, 0.0f);

        // **Konteks disetel lebih dulu, baru mesh sapuannya dibuat darinya.**
        // Urutan sebaliknya membangun silinder satuan dari kerangka sumbu yang
        // belum berisi apa-apa, dan kegagalannya tidak terlihat sebagai galat:
        // query jalan tidak pernah menemukan tanah, dan mobil hanya diam di
        // tempat dengan gas penuh.
        impl_->vehicleContext.setToDefault();
        // Kerangka sumbu disamakan dengan yang dipakai backend — Y di atas,
        // bukan Z bawaan PhysX.
        impl_->vehicleContext.frame = SimVehicleFrame();
        impl_->vehicleContext.gravity = ToPx(desc.gravity);
        impl_->unitCylinderMesh = physx::vehicle2::PxVehicleUnitCylinderSweepMeshCreate(
            impl_->vehicleContext.frame, *impl_->physics, *impl_->cookingParams);
        impl_->vehicleContext.physxScene = impl_->scene;
        impl_->vehicleContext.physxUnitCylinderSweepMesh = impl_->unitCylinderMesh;
        impl_->vehicleContext.physxActorUpdateMode =
            physx::vehicle2::PxVehiclePhysXActorUpdateMode::eAPPLY_VELOCITY;
    } else {
        SIM_WARN("Physics", "the PhysX vehicle extension failed to start; vehicles are off");
    }

    return true;
}

void PhysicsWorld::Destroy() { impl_.reset(); }

bool PhysicsWorld::IsValid() const { return impl_ != nullptr && impl_->scene != nullptr; }

const std::string& PhysicsWorld::Error() const {
    static const std::string kNone;
    return impl_ != nullptr ? impl_->error : kNone;
}

BodyHandle PhysicsWorld::AddBody(const BodyDesc& desc) {
    if (!IsValid()) {
        return BodyHandle::Invalid;
    }
    physx::PxPhysics* physics = impl_->physics;

    physx::PxMaterial* material = physics->createMaterial(
        desc.material.staticFriction, desc.material.dynamicFriction, desc.material.restitution);
    if (material == nullptr) {
        impl_->error = "createMaterial failed";
        return BodyHandle::Invalid;
    }

    // Bidang tak hingga hanya sah untuk benda statis, dan PhysX menolaknya di
    // tempat lain — ditolak di sini supaya pesannya menyebut sebabnya.
    if (desc.shape.kind == ShapeKind::Plane && desc.kind != BodyKind::Static) {
        impl_->error = "an infinite plane can only be a static body";
        material->release();
        return BodyHandle::Invalid;
    }

    physx::PxShape* shape = nullptr;
    switch (desc.shape.kind) {
        case ShapeKind::Box:
            shape = physics->createShape(
                physx::PxBoxGeometry(ToPx(glm::max(desc.shape.halfExtents, Vec3(1e-4f)))),
                *material);
            break;
        case ShapeKind::Sphere:
            shape = physics->createShape(
                physx::PxSphereGeometry(std::max(desc.shape.radius, 1e-4f)), *material);
            break;
        case ShapeKind::Capsule:
            shape = physics->createShape(
                physx::PxCapsuleGeometry(std::max(desc.shape.radius, 1e-4f),
                                         std::max(desc.shape.halfExtents.x, 1e-4f)),
                *material);
            break;
        case ShapeKind::Plane:
            shape = physics->createShape(physx::PxPlaneGeometry(), *material);
            break;
    }
    if (shape == nullptr) {
        impl_->error = "createShape failed";
        material->release();
        return BodyHandle::Invalid;
    }
    shape->setLocalPose(ToPx(desc.shape.localPosition, desc.shape.localRotation));

    // Lapisan disimpan di `word0` data filter query. **Query filter, bukan
    // simulation filter** — keduanya struktur terpisah di PhysX, dan yang
    // menentukan benda mana bertabrakan dengan benda mana adalah yang kedua.
    // Menaruh lapisan di sana akan mengubah simulasinya, padahal yang diminta
    // hanya menyaring pertanyaan.
    physx::PxFilterData queryFilter;
    queryFilter.word0 = desc.layer;
    shape->setQueryFilterData(queryFilter);

    const physx::PxTransform pose = ToPx(desc.position, desc.rotation);
    physx::PxRigidActor* actor = nullptr;
    if (desc.kind == BodyKind::Static) {
        actor = physics->createRigidStatic(pose);
    } else {
        physx::PxRigidDynamic* dynamic = physics->createRigidDynamic(pose);
        if (dynamic != nullptr) {
            dynamic->setLinearDamping(desc.linearDamping);
            dynamic->setAngularDamping(desc.angularDamping);
            dynamic->setRigidBodyFlag(physx::PxRigidBodyFlag::eKINEMATIC,
                                      desc.kind == BodyKind::Kinematic);
            dynamic->setActorFlag(physx::PxActorFlag::eDISABLE_GRAVITY, false);
            if (!desc.allowSleeping) {
                dynamic->setSleepThreshold(0.0f);
            }
        }
        actor = dynamic;
    }
    if (actor == nullptr) {
        impl_->error = "createRigidActor failed";
        shape->release();
        material->release();
        return BodyHandle::Invalid;
    }

    actor->attachShape(*shape);
    if (auto* dynamic = actor->is<physx::PxRigidDynamic>();
        dynamic != nullptr && desc.kind == BodyKind::Dynamic) {
        // Massa nol berarti "hitung dari volume", bukan "tanpa massa" — benda
        // dinamis bermassa nol adalah galat PhysX, bukan benda tak berbobot.
        if (desc.mass > 0.0f) {
            physx::PxRigidBodyExt::setMassAndUpdateInertia(*dynamic, desc.mass);
        } else {
            physx::PxRigidBodyExt::updateMassAndInertia(*dynamic, desc.density);
        }
    }

    // Shape dan material dipegang aktor sekarang; rujukan lokal dilepas supaya
    // keduanya ikut mati bersama aktornya.
    shape->release();
    material->release();

    impl_->scene->addActor(*actor);
    const uint64_t handle = impl_->nextHandle++;
    impl_->bodies.emplace(handle, actor);
    impl_->handlesByActor.emplace(actor, handle);
    return static_cast<BodyHandle>(handle);
}

void PhysicsWorld::RemoveBody(BodyHandle body) {
    if (!IsValid()) {
        return;
    }
    const auto found = impl_->bodies.find(static_cast<uint64_t>(body));
    if (found == impl_->bodies.end()) {
        return;
    }
    if (found->second != nullptr) {
        // **Sendinya dulu.** Sendi yang masih memegang aktor yang sudah dilepas
        // adalah pointer menggantung yang tidak crash di sini melainkan di
        // langkah simulasi berikutnya — sejauh mungkin dari sebabnya. Ini pula
        // kriteria terima P3 yang diuji dengan benar-benar menghapus entity,
        // bukan dengan membaca kode.
        const std::size_t releasedJoints = impl_->ReleaseJointsTouching(found->second);
        if (releasedJoints > 0) {
            SIM_INFO("Physics", "removing a body released {} joint(s) that held it",
                     releasedJoints);
        }
        impl_->handlesByActor.erase(found->second);
        impl_->scene->removeActor(*found->second);
        found->second->release();
    }
    impl_->bodies.erase(found);
}

bool PhysicsWorld::IsAlive(BodyHandle body) const {
    return impl_ != nullptr && impl_->bodies.count(static_cast<uint64_t>(body)) != 0;
}

std::size_t PhysicsWorld::BodyCount() const {
    return impl_ != nullptr ? impl_->bodies.size() : 0;
}

bool PhysicsWorld::ReadState(BodyHandle body, BodyState& out) const {
    if (impl_ == nullptr) {
        return false;
    }
    const auto found = impl_->bodies.find(static_cast<uint64_t>(body));
    if (found == impl_->bodies.end() || found->second == nullptr) {
        return false;
    }
    const physx::PxTransform pose = found->second->getGlobalPose();
    out = BodyState{};
    out.position = FromPx(pose.p);
    out.rotation = FromPx(pose.q);
    if (const auto* dynamic = found->second->is<physx::PxRigidDynamic>(); dynamic != nullptr) {
        out.linearVelocity = FromPx(dynamic->getLinearVelocity());
        out.angularVelocity = FromPx(dynamic->getAngularVelocity());
        out.sleeping = dynamic->isSleeping();
    }
    return true;
}

bool PhysicsWorld::MoveKinematic(BodyHandle body, const Vec3& position, const Quat& rotation) {
    if (impl_ == nullptr) {
        return false;
    }
    const auto found = impl_->bodies.find(static_cast<uint64_t>(body));
    if (found == impl_->bodies.end()) {
        return false;
    }
    auto* dynamic = found->second->is<physx::PxRigidDynamic>();
    if (dynamic == nullptr ||
        !dynamic->getRigidBodyFlags().isSet(physx::PxRigidBodyFlag::eKINEMATIC)) {
        impl_->error = "MoveKinematic called on a body that is not kinematic";
        return false;
    }
    dynamic->setKinematicTarget(ToPx(position, rotation));
    return true;
}

bool PhysicsWorld::Teleport(BodyHandle body, const Vec3& position, const Quat& rotation) {
    if (impl_ == nullptr) {
        return false;
    }
    const auto found = impl_->bodies.find(static_cast<uint64_t>(body));
    if (found == impl_->bodies.end() || found->second == nullptr) {
        return false;
    }
    found->second->setGlobalPose(ToPx(position, rotation));
    if (auto* dynamic = found->second->is<physx::PxRigidDynamic>();
        dynamic != nullptr &&
        !dynamic->getRigidBodyFlags().isSet(physx::PxRigidBodyFlag::eKINEMATIC)) {
        // Kecepatannya ikut dinolkan: benda yang dipindahkan tapi masih membawa
        // kecepatan lamanya akan melesat dari tempat barunya.
        dynamic->setLinearVelocity(physx::PxVec3(0.0f));
        dynamic->setAngularVelocity(physx::PxVec3(0.0f));
    }
    return true;
}

void PhysicsWorld::Step(uint32_t steps) {
    if (!IsValid()) {
        return;
    }
    // Ditandai untuk seluruh rangkaian, bukan per langkah — dan itu **lebih
    // ketat daripada yang dituntut PhysX**, yang hanya melarang query di antara
    // `simulate` dan `fetchResults`. Sela di antara dua langkah sebenarnya sah.
    //
    // Dipilih begitu karena selanya berdurasi mikrodetik sementara salah
    // menempatkan batasnya berakibat kerusakan senyap, dan karena `steps > 1`
    // hanya terjadi di test dan jalur headless — bukan di frame editor, yang
    // memanggil `Advance` dan hampir selalu berlangkah satu.
    impl_->stepping.store(true, std::memory_order_release);
    for (uint32_t i = 0; i < steps; ++i) {
        // **Kendaraan dilangkahkan sebelum scene, di dalam langkah yang sama.**
        // Ia menghitung gaya suspensi dan ban lalu menuliskannya ke aktornya;
        // menjalankannya sesudah `simulate` berarti gaya frame ini baru terasa
        // pada frame berikutnya, dan mobil terasa seperti dikemudikan lewat pos.
        for (auto& [handle, vehicle] : impl_->vehicles) {
            if (vehicle != nullptr) {
                vehicle->Step(impl_->desc.fixedTimeStep, impl_->vehicleContext);
            }
        }
        impl_->scene->simulate(impl_->desc.fixedTimeStep);
        // Blocking: penulisan transform ke `World` terjadi di main thread, dan
        // menunggu di sini jauh lebih sederhana daripada satu keadaan lagi yang
        // harus dijaga di antara dua frame. Solvernya sendiri tetap paralel
        // lewat `PxCpuDispatcher`.
        impl_->scene->fetchResults(/*block=*/true);
        ++impl_->stepCount;
    }
    impl_->stepping.store(false, std::memory_order_release);
}

uint32_t PhysicsWorld::Advance(float deltaSeconds) {
    if (!IsValid() || !(deltaSeconds > 0.0f)) {
        return 0;
    }
    impl_->accumulator += static_cast<double>(deltaSeconds);
    const double step = static_cast<double>(impl_->desc.fixedTimeStep);

    uint32_t taken = 0;
    while (impl_->accumulator >= step && taken < impl_->desc.maxStepsPerAdvance) {
        Step(1);
        impl_->accumulator -= step;
        ++taken;
    }

    // **Sisa yang tidak terkejar dibuang, bukan ditumpuk.** Menumpuknya membuat
    // satu frame tersendat memicu langkah tambahan di frame berikutnya, yang
    // membuatnya tersendat juga — spiral yang berakhir sebagai editor yang
    // tampak menggantung. Melewatkan waktu lebih baik daripada berhenti
    // merespons.
    if (impl_->accumulator > step * static_cast<double>(impl_->desc.maxStepsPerAdvance)) {
        impl_->accumulator = 0.0;
    }
    return taken;
}

float PhysicsWorld::Alpha() const {
    if (!IsValid()) {
        return 0.0f;
    }
    const double alpha = impl_->accumulator / static_cast<double>(impl_->desc.fixedTimeStep);
    return std::clamp(static_cast<float>(alpha), 0.0f, 1.0f);
}

uint64_t PhysicsWorld::StepCount() const { return impl_ != nullptr ? impl_->stepCount : 0; }

// --- kendaraan ---------------------------------------------------------------

VehicleHandle PhysicsWorld::AddVehicle(const VehicleDesc& desc) {
    if (!IsValid()) {
        return VehicleHandle::Invalid;
    }
    if (!impl_->vehicleExtensionReady) {
        impl_->error = "the PhysX vehicle extension is not available";
        return VehicleHandle::Invalid;
    }

    auto vehicle = std::make_unique<VehicleInstance>();
    if (!vehicle->Create(desc, *impl_->physics, *impl_->cookingParams, *impl_->vehicleMaterial,
                         impl_->error)) {
        return VehicleHandle::Invalid;
    }
    vehicle->AddToScene(*impl_->scene);

    const uint64_t handle = impl_->nextVehicleHandle++;
    // Chassis-nya ikut terdaftar sebagai benda biasa, sehingga scene query dan
    // sendi bisa menyebutnya — roda tidak, karena roda memang bukan benda tegar.
    physx::PxRigidActor* chassis = vehicle->RigidBody();
    if (chassis != nullptr) {
        impl_->bodies.emplace(impl_->nextHandle, chassis);
        impl_->handlesByActor.emplace(chassis, impl_->nextHandle);
        ++impl_->nextHandle;
    }
    impl_->vehicles.emplace(handle, std::move(vehicle));
    return static_cast<VehicleHandle>(handle);
}

void PhysicsWorld::RemoveVehicle(VehicleHandle vehicle) {
    if (!IsValid()) {
        return;
    }
    const auto found = impl_->vehicles.find(static_cast<uint64_t>(vehicle));
    if (found == impl_->vehicles.end()) {
        return;
    }
    if (found->second != nullptr) {
        physx::PxRigidBody* chassis = found->second->RigidBody();
        if (chassis != nullptr) {
            const auto handle = impl_->handlesByActor.find(chassis);
            if (handle != impl_->handlesByActor.end()) {
                impl_->ReleaseJointsTouching(chassis);
                impl_->bodies.erase(handle->second);
                impl_->handlesByActor.erase(handle);
            }
        }
        found->second->RemoveFromScene(*impl_->scene);
        found->second->Destroy();
    }
    impl_->vehicles.erase(found);
}

bool PhysicsWorld::IsVehicleAlive(VehicleHandle vehicle) const {
    return impl_ != nullptr && impl_->vehicles.count(static_cast<uint64_t>(vehicle)) != 0;
}

std::size_t PhysicsWorld::VehicleCount() const {
    return impl_ != nullptr ? impl_->vehicles.size() : 0;
}

bool PhysicsWorld::SetVehicleInput(VehicleHandle vehicle, const VehicleInput& input) {
    if (!IsValid()) {
        return false;
    }
    const auto found = impl_->vehicles.find(static_cast<uint64_t>(vehicle));
    if (found == impl_->vehicles.end() || found->second == nullptr) {
        return false;
    }
    found->second->SetInput(input);
    return true;
}

bool PhysicsWorld::ReadVehicleState(VehicleHandle vehicle, VehicleState& out) const {
    out = VehicleState{};
    if (impl_ == nullptr) {
        return false;
    }
    const auto found = impl_->vehicles.find(static_cast<uint64_t>(vehicle));
    if (found == impl_->vehicles.end() || found->second == nullptr) {
        return false;
    }
    found->second->ReadState(out);
    return true;
}

BodyHandle PhysicsWorld::VehicleChassis(VehicleHandle vehicle) const {
    if (impl_ == nullptr) {
        return BodyHandle::Invalid;
    }
    const auto found = impl_->vehicles.find(static_cast<uint64_t>(vehicle));
    if (found == impl_->vehicles.end() || found->second == nullptr) {
        return BodyHandle::Invalid;
    }
    const auto handle = impl_->handlesByActor.find(found->second->RigidBody());
    return handle == impl_->handlesByActor.end() ? BodyHandle::Invalid
                                                 : static_cast<BodyHandle>(handle->second);
}

// --- articulation ------------------------------------------------------------

namespace {

physx::PxArticulationJointType::Enum ToPxJointType(ArticulationJointKind kind) {
    switch (kind) {
        case ArticulationJointKind::Fixed: return physx::PxArticulationJointType::eFIX;
        case ArticulationJointKind::Revolute: return physx::PxArticulationJointType::eREVOLUTE;
        case ArticulationJointKind::Prismatic: return physx::PxArticulationJointType::ePRISMATIC;
        case ArticulationJointKind::Spherical: return physx::PxArticulationJointType::eSPHERICAL;
    }
    return physx::PxArticulationJointType::eSPHERICAL;
}

}  // namespace

ArticulationHandle PhysicsWorld::AddArticulation(const ArticulationDesc& desc) {
    if (!IsValid()) {
        return ArticulationHandle::Invalid;
    }
    if (desc.links.empty()) {
        impl_->error = "an articulation needs at least a root link";
        return ArticulationHandle::Invalid;
    }
    if (desc.links.front().parent != kArticulationRootParent) {
        impl_->error = "the first link of an articulation must be the root";
        return ArticulationHandle::Invalid;
    }
    for (std::size_t i = 1; i < desc.links.size(); ++i) {
        const int parent = desc.links[i].parent;
        // Induk yang tidak mendahului anaknya berarti siklus atau rujukan ke
        // link yang belum ada. PhysX menjawab keduanya dengan crash, bukan
        // dengan pesan — jadi diperiksa di sini.
        if (parent < 0 || parent >= static_cast<int>(i)) {
            impl_->error = "articulation links must list their parent before themselves";
            return ArticulationHandle::Invalid;
        }
    }

    physx::PxArticulationReducedCoordinate* articulation =
        impl_->physics->createArticulationReducedCoordinate();
    if (articulation == nullptr) {
        impl_->error = "createArticulationReducedCoordinate failed";
        return ArticulationHandle::Invalid;
    }

    articulation->setSolverIterationCounts(desc.solverPositionIterations,
                                           desc.solverVelocityIterations);
    articulation->setArticulationFlag(physx::PxArticulationFlag::eFIX_BASE, desc.fixBase);
    articulation->setArticulationFlag(physx::PxArticulationFlag::eDISABLE_SELF_COLLISION,
                                      !desc.selfCollision);
    if (!desc.allowSleeping) {
        articulation->setSleepThreshold(0.0f);
    }

    std::vector<physx::PxArticulationLink*> links;
    links.reserve(desc.links.size());

    for (const ArticulationLinkDesc& linkDesc : desc.links) {
        physx::PxArticulationLink* parentLink =
            linkDesc.parent == kArticulationRootParent
                ? nullptr
                : links[static_cast<std::size_t>(linkDesc.parent)];

        physx::PxArticulationLink* link = articulation->createLink(
            parentLink, ToPx(linkDesc.position, linkDesc.rotation));
        if (link == nullptr) {
            impl_->error = "creating an articulation link failed";
            articulation->release();
            return ArticulationHandle::Invalid;
        }

        physx::PxMaterial* material = impl_->physics->createMaterial(
            linkDesc.material.staticFriction, linkDesc.material.dynamicFriction,
            linkDesc.material.restitution);
        physx::PxGeometryHolder geometry;
        switch (linkDesc.shape.kind) {
            case ShapeKind::Box:
                geometry = physx::PxBoxGeometry(
                    ToPx(glm::max(linkDesc.shape.halfExtents, Vec3(1e-4f))));
                break;
            case ShapeKind::Sphere:
                geometry = physx::PxSphereGeometry(std::max(linkDesc.shape.radius, 1e-4f));
                break;
            case ShapeKind::Capsule:
                geometry = physx::PxCapsuleGeometry(
                    std::max(linkDesc.shape.radius, 1e-4f),
                    std::max(linkDesc.shape.halfExtents.x, 1e-4f));
                break;
            case ShapeKind::Plane:
                // Bidang tak hingga tidak bisa menjadi bagian tubuh yang
                // bergerak; ditolak di sini alih-alih dibiarkan PhysX menegur.
                impl_->error = "an articulation link cannot be an infinite plane";
                material->release();
                articulation->release();
                return ArticulationHandle::Invalid;
        }

        physx::PxShape* shape =
            physx::PxRigidActorExt::createExclusiveShape(*link, geometry.any(), *material);
        material->release();
        if (shape == nullptr) {
            impl_->error = "attaching a shape to an articulation link failed";
            articulation->release();
            return ArticulationHandle::Invalid;
        }
        shape->setLocalPose(ToPx(linkDesc.shape.localPosition, linkDesc.shape.localRotation));

        if (linkDesc.mass > 0.0f) {
            physx::PxRigidBodyExt::setMassAndUpdateInertia(*link, linkDesc.mass);
        } else {
            physx::PxRigidBodyExt::updateMassAndInertia(*link, linkDesc.density);
        }

        if (parentLink != nullptr) {
            physx::PxArticulationJointReducedCoordinate* joint = link->getInboundJoint();
            if (joint == nullptr) {
                impl_->error = "an articulation link has no inbound joint";
                articulation->release();
                return ArticulationHandle::Invalid;
            }
            joint->setJointType(ToPxJointType(linkDesc.joint));
            joint->setParentPose(ToPx(linkDesc.parentAnchor, linkDesc.parentFrame));
            joint->setChildPose(ToPx(linkDesc.childAnchor, linkDesc.childFrame));

            // Sumbu yang tidak disebut jenis sendinya tetap terkunci — itu
            // bawaan PhysX, dan yang membuat sendi revolute benar-benar hanya
            // satu derajat kebebasan.
            const auto axisMotion = [&](physx::PxArticulationAxis::Enum axis, float lower,
                                        float upper) {
                if (!linkDesc.limitEnabled) {
                    joint->setMotion(axis, physx::PxArticulationMotion::eFREE);
                    return;
                }
                joint->setMotion(axis, physx::PxArticulationMotion::eLIMITED);
                joint->setLimitParams(axis, physx::PxArticulationLimit(lower, upper));
            };

            switch (linkDesc.joint) {
                case ArticulationJointKind::Fixed:
                    break;
                case ArticulationJointKind::Revolute:
                    axisMotion(physx::PxArticulationAxis::eTWIST, linkDesc.lowerLimit,
                               linkDesc.upperLimit);
                    break;
                case ArticulationJointKind::Prismatic:
                    axisMotion(physx::PxArticulationAxis::eX, linkDesc.lowerLimit,
                               linkDesc.upperLimit);
                    break;
                case ArticulationJointKind::Spherical:
                    axisMotion(physx::PxArticulationAxis::eSWING1, linkDesc.lowerLimit,
                               linkDesc.upperLimit);
                    axisMotion(physx::PxArticulationAxis::eSWING2, -linkDesc.swing2Limit,
                               linkDesc.swing2Limit);
                    break;
            }

            if (desc.jointFriction > 0.0f) {
                joint->setFrictionCoefficient(desc.jointFriction);
            }
        }

        links.push_back(link);
    }

    // **Seluruh link dibuat sebelum articulation masuk ke scene.** PhysX tidak
    // mengizinkan strukturnya berubah sesudahnya, dan menambah link ke
    // articulation yang sudah masuk gagal secara diam-diam.
    impl_->scene->addArticulation(*articulation);

    const uint64_t handle = impl_->nextArticulationHandle++;
    impl_->articulations.emplace(handle, articulation);
    impl_->articulationLinks.emplace(handle, std::move(links));
    return static_cast<ArticulationHandle>(handle);
}

void PhysicsWorld::RemoveArticulation(ArticulationHandle articulation) {
    if (!IsValid()) {
        return;
    }
    const auto found = impl_->articulations.find(static_cast<uint64_t>(articulation));
    if (found == impl_->articulations.end()) {
        return;
    }
    if (found->second != nullptr) {
        impl_->scene->removeArticulation(*found->second);
        found->second->release();
    }
    impl_->articulations.erase(found);
    impl_->articulationLinks.erase(static_cast<uint64_t>(articulation));
}

bool PhysicsWorld::IsArticulationAlive(ArticulationHandle articulation) const {
    return impl_ != nullptr &&
           impl_->articulations.count(static_cast<uint64_t>(articulation)) != 0;
}

std::size_t PhysicsWorld::ArticulationCount() const {
    return impl_ != nullptr ? impl_->articulations.size() : 0;
}

std::size_t PhysicsWorld::ArticulationLinkCount(ArticulationHandle articulation) const {
    if (impl_ == nullptr) {
        return 0;
    }
    const auto found = impl_->articulationLinks.find(static_cast<uint64_t>(articulation));
    return found == impl_->articulationLinks.end() ? 0 : found->second.size();
}

bool PhysicsWorld::ReadLinkState(ArticulationHandle articulation, std::size_t link,
                                 BodyState& out) const {
    out = BodyState{};
    if (impl_ == nullptr) {
        return false;
    }
    const auto found = impl_->articulationLinks.find(static_cast<uint64_t>(articulation));
    if (found == impl_->articulationLinks.end() || link >= found->second.size()) {
        return false;
    }
    physx::PxArticulationLink* actor = found->second[link];
    if (actor == nullptr) {
        return false;
    }
    const physx::PxTransform pose = actor->getGlobalPose();
    out.position = FromPx(pose.p);
    out.rotation = FromPx(pose.q);
    out.linearVelocity = FromPx(actor->getLinearVelocity());
    out.angularVelocity = FromPx(actor->getAngularVelocity());
    return true;
}

// --- sendi -------------------------------------------------------------------

JointHandle PhysicsWorld::AddJoint(const JointDesc& desc) {
    if (!IsValid()) {
        return JointHandle::Invalid;
    }

    // Ujung yang `Invalid` berarti dunia, dan PhysX menyatakannya sebagai aktor
    // null. Yang bukan `Invalid` tapi tidak dikenal adalah kesalahan, bukan
    // dunia — membiarkannya lolos menjadikan handle yang salah ketik sebagai
    // sendi yang diam-diam menggantung ke ruang kosong.
    const auto resolve = [this](BodyHandle body, physx::PxRigidActor*& out) {
        if (body == BodyHandle::Invalid) {
            out = nullptr;
            return true;
        }
        const auto found = impl_->bodies.find(static_cast<uint64_t>(body));
        if (found == impl_->bodies.end()) {
            return false;
        }
        out = found->second;
        return true;
    };

    physx::PxRigidActor* actorA = nullptr;
    physx::PxRigidActor* actorB = nullptr;
    if (!resolve(desc.bodyA, actorA) || !resolve(desc.bodyB, actorB)) {
        impl_->error = "a joint referenced a body that does not exist";
        return JointHandle::Invalid;
    }
    if (actorA == nullptr && actorB == nullptr) {
        impl_->error = "a joint needs at least one real body; both ends were the world";
        return JointHandle::Invalid;
    }

    const physx::PxTransform frameA = ToPx(desc.localAnchorA, desc.localRotationA);
    const physx::PxTransform frameB = ToPx(desc.localAnchorB, desc.localRotationB);

    physx::PxJoint* joint = nullptr;
    switch (desc.kind) {
        case JointKind::Fixed:
            joint = physx::PxFixedJointCreate(*impl_->physics, actorA, frameA, actorB, frameB);
            break;

        case JointKind::Revolute: {
            physx::PxRevoluteJoint* revolute =
                physx::PxRevoluteJointCreate(*impl_->physics, actorA, frameA, actorB, frameB);
            if (revolute != nullptr && desc.limit.enabled) {
                physx::PxJointAngularLimitPair limit(desc.limit.lower, desc.limit.upper);
                if (desc.limit.stiffness > 0.0f) {
                    limit = physx::PxJointAngularLimitPair(
                        desc.limit.lower, desc.limit.upper,
                        physx::PxSpring(desc.limit.stiffness, desc.limit.damping));
                }
                revolute->setLimit(limit);
                revolute->setRevoluteJointFlag(physx::PxRevoluteJointFlag::eLIMIT_ENABLED, true);
            }
            joint = revolute;
            break;
        }

        case JointKind::Prismatic: {
            physx::PxPrismaticJoint* prismatic =
                physx::PxPrismaticJointCreate(*impl_->physics, actorA, frameA, actorB, frameB);
            if (prismatic != nullptr && desc.limit.enabled) {
                physx::PxJointLinearLimitPair limit(impl_->physics->getTolerancesScale(),
                                                    desc.limit.lower, desc.limit.upper);
                if (desc.limit.stiffness > 0.0f) {
                    limit = physx::PxJointLinearLimitPair(
                        desc.limit.lower, desc.limit.upper,
                        physx::PxSpring(desc.limit.stiffness, desc.limit.damping));
                }
                prismatic->setLimit(limit);
                prismatic->setPrismaticJointFlag(physx::PxPrismaticJointFlag::eLIMIT_ENABLED,
                                                 true);
            }
            joint = prismatic;
            break;
        }

        case JointKind::Spherical: {
            physx::PxSphericalJoint* spherical =
                physx::PxSphericalJointCreate(*impl_->physics, actorA, frameA, actorB, frameB);
            if (spherical != nullptr && desc.limit.enabled) {
                // Kerucutnya simetris di sini: `upper` adalah setengah-sudut
                // bukaannya pada kedua sumbu. Kerucut elips yang dimiliki PhysX
                // menuntut dua sudut, dan `JointLimit` tidak punya tempat untuk
                // menyebutkan keduanya tanpa berarti hal lain untuk jenis sendi
                // yang lain.
                const float half = std::max(desc.limit.upper, 1e-3f);
                spherical->setLimitCone(physx::PxJointLimitCone(half, half));
                spherical->setSphericalJointFlag(physx::PxSphericalJointFlag::eLIMIT_ENABLED,
                                                 true);
            }
            joint = spherical;
            break;
        }

        case JointKind::D6: {
            physx::PxD6Joint* d6 =
                physx::PxD6JointCreate(*impl_->physics, actorA, frameA, actorB, frameB);
            if (d6 != nullptr) {
                const auto motion = [](bool free) {
                    return free ? physx::PxD6Motion::eFREE : physx::PxD6Motion::eLOCKED;
                };
                d6->setMotion(physx::PxD6Axis::eX, motion(desc.d6.freeLinearX));
                d6->setMotion(physx::PxD6Axis::eY, motion(desc.d6.freeLinearY));
                d6->setMotion(physx::PxD6Axis::eZ, motion(desc.d6.freeLinearZ));
                d6->setMotion(physx::PxD6Axis::eTWIST, motion(desc.d6.freeAngularX));
                d6->setMotion(physx::PxD6Axis::eSWING1, motion(desc.d6.freeAngularY));
                d6->setMotion(physx::PxD6Axis::eSWING2, motion(desc.d6.freeAngularZ));
            }
            joint = d6;
            break;
        }
    }

    if (joint == nullptr) {
        impl_->error = "creating the joint failed";
        return JointHandle::Invalid;
    }

    joint->setConstraintFlag(physx::PxConstraintFlag::eCOLLISION_ENABLED,
                             desc.collisionEnabled);
    if (desc.breakForce > 0.0f || desc.breakTorque > 0.0f) {
        // Nol pada salah satunya berarti "tidak patah karena yang ini", bukan
        // "patah seketika" — jadi yang tidak disetel dibuat tak hingga.
        joint->setBreakForce(desc.breakForce > 0.0f ? desc.breakForce : PX_MAX_F32,
                             desc.breakTorque > 0.0f ? desc.breakTorque : PX_MAX_F32);
    }

    const uint64_t handle = impl_->nextJointHandle++;
    impl_->joints.emplace(handle, joint);
    return static_cast<JointHandle>(handle);
}

void PhysicsWorld::RemoveJoint(JointHandle joint) {
    if (!IsValid()) {
        return;
    }
    const auto found = impl_->joints.find(static_cast<uint64_t>(joint));
    if (found == impl_->joints.end()) {
        return;
    }
    if (found->second != nullptr) {
        found->second->release();
    }
    impl_->joints.erase(found);
}

bool PhysicsWorld::IsJointAlive(JointHandle joint) const {
    return impl_ != nullptr && impl_->joints.count(static_cast<uint64_t>(joint)) != 0;
}

std::size_t PhysicsWorld::JointCount() const {
    return impl_ != nullptr ? impl_->joints.size() : 0;
}

bool PhysicsWorld::ReadJointState(JointHandle joint, JointState& out) const {
    out = JointState{};
    if (!IsValid()) {
        return false;
    }
    const auto found = impl_->joints.find(static_cast<uint64_t>(joint));
    if (found == impl_->joints.end() || found->second == nullptr) {
        return false;
    }

    physx::PxJoint* handle = found->second;
    out.alive = !handle->getConstraintFlags().isSet(physx::PxConstraintFlag::eBROKEN);
    if (auto* revolute = handle->is<physx::PxRevoluteJoint>(); revolute != nullptr) {
        out.position = revolute->getAngle();
    } else if (auto* prismatic = handle->is<physx::PxPrismaticJoint>(); prismatic != nullptr) {
        out.position = prismatic->getPosition();
    }
    return true;
}

// --- scene query ------------------------------------------------------------

namespace {

/// Menyaring calon hit berdasarkan lapisan, sebelum perpotongannya dihitung.
///
/// **Prefilter, bukan postfilter**, dan bedanya bukan gaya: yang ditolak di
/// prefilter tidak pernah menjalani uji perpotongan sama sekali. Untuk ray yang
/// melintasi adegan penuh benda yang tidak diminati — hampir setiap raycast
/// gameplay — itulah selisih antara memeriksa segelintir benda dan memeriksa
/// semuanya.
class LayerFilter final : public physx::PxQueryFilterCallback {
public:
    physx::PxQueryHitType::Enum preFilter(const physx::PxFilterData& filterData,
                                          const physx::PxShape* shape,
                                          const physx::PxRigidActor* /*actor*/,
                                          physx::PxHitFlags& /*flags*/) override {
        // Mask kosong tidak perlu ditangani terpisah: `apa pun & 0` adalah nol,
        // jadi `kNoLayers` sudah menolak segalanya lewat uji yang sama.
        const physx::PxFilterData shapeData = shape->getQueryFilterData();
        return (shapeData.word0 & filterData.word0) == 0 ? physx::PxQueryHitType::eNONE
                                                         : physx::PxQueryHitType::eBLOCK;
    }

    physx::PxQueryHitType::Enum postFilter(const physx::PxFilterData& /*filterData*/,
                                           const physx::PxQueryHit& /*hit*/,
                                           const physx::PxShape* /*shape*/,
                                           const physx::PxRigidActor* /*actor*/) override {
        return physx::PxQueryHitType::eBLOCK;
    }
};

physx::PxQueryFilterData MakeFilterData(const QueryFilter& filter) {
    physx::PxQueryFilterData data;
    data.data.word0 = filter.layers;
    data.flags = physx::PxQueryFlag::ePREFILTER;
    if (filter.hitStatic) {
        data.flags |= physx::PxQueryFlag::eSTATIC;
    }
    if (filter.hitDynamic) {
        data.flags |= physx::PxQueryFlag::eDYNAMIC;
    }
    return data;
}

}  // namespace

/// Syarat yang sama untuk seluruh query, dikumpulkan supaya tidak ada satu pun
/// jalur yang lupa memeriksanya.
///
/// Arah bernorma nol dikembalikan sebagai penolakan, bukan dinormalkan menjadi
/// sesuatu: ray tanpa arah tidak punya jawaban yang benar, dan menebaknya
/// menghasilkan hit di tempat yang tak seorang pun menembak.
#define SIM_PHYSICS_QUERY_GUARD(returnValue)                                                    \
    if (!IsValid()) {                                                                           \
        return returnValue;                                                                     \
    }                                                                                           \
    if (impl_->stepping.load(std::memory_order_acquire)) {                                      \
        SIM_ERROR("Physics",                                                                    \
                  "a scene query ran while the simulation was stepping; PhysX would return "    \
                  "half-updated state, so it was refused");                                     \
        return returnValue;                                                                     \
    }

bool PhysicsWorld::Raycast(const Vec3& origin, const Vec3& direction, float maxDistance,
                           RayHit& out, const QueryFilter& filter) const {
    out = RayHit{};
    SIM_PHYSICS_QUERY_GUARD(false)

    const float length = glm::length(direction);
    if (!(length > 0.0f) || !(maxDistance > 0.0f)) {
        return false;
    }

    physx::PxRaycastBuffer hit;
    LayerFilter layerFilter;
    const bool found = impl_->scene->raycast(
        ToPx(origin), ToPx(direction / length), maxDistance, hit,
        physx::PxHitFlag::ePOSITION | physx::PxHitFlag::eNORMAL, MakeFilterData(filter),
        &layerFilter);
    if (!found || !hit.hasBlock) {
        return false;
    }

    const auto handle = impl_->handlesByActor.find(hit.block.actor);
    out.body = handle == impl_->handlesByActor.end()
                   ? BodyHandle::Invalid
                   : static_cast<BodyHandle>(handle->second);
    out.position = FromPx(hit.block.position);
    out.normal = FromPx(hit.block.normal);
    out.distance = hit.block.distance;
    return true;
}

std::size_t PhysicsWorld::RaycastAll(const Vec3& origin, const Vec3& direction, float maxDistance,
                                     std::vector<RayHit>& out, const QueryFilter& filter) const {
    out.clear();
    SIM_PHYSICS_QUERY_GUARD(0)

    const float length = glm::length(direction);
    if (!(length > 0.0f) || !(maxDistance > 0.0f)) {
        return 0;
    }

    // Batas atas yang tetap: PhysX menulis ke buffer yang kita sediakan, dan
    // alokasi per-query di jalur yang dipanggil ratusan kali per frame adalah
    // biaya yang tidak perlu. Yang melampauinya dilaporkan alih-alih dipotong
    // diam-diam — daftar yang terpotong tanpa pemberitahuan terbaca sebagai
    // dinding yang hilang.
    constexpr std::size_t kMaxHits = 64;
    physx::PxRaycastHit buffer[kMaxHits];
    physx::PxRaycastBuffer hits(buffer, static_cast<physx::PxU32>(kMaxHits));

    LayerFilter layerFilter;
    physx::PxQueryFilterData data = MakeFilterData(filter);
    // eNO_BLOCK: setiap sentuhan dikumpulkan alih-alih berhenti di yang pertama.
    data.flags |= physx::PxQueryFlag::eNO_BLOCK;

    impl_->scene->raycast(ToPx(origin), ToPx(direction / length), maxDistance, hits,
                          physx::PxHitFlag::ePOSITION | physx::PxHitFlag::eNORMAL, data,
                          &layerFilter);

    if (hits.nbTouches >= kMaxHits) {
        SIM_WARN("Physics", "a ray hit at least {} shapes; the list stops there", kMaxHits);
    }

    out.reserve(hits.nbTouches);
    for (physx::PxU32 i = 0; i < hits.nbTouches; ++i) {
        const physx::PxRaycastHit& touch = hits.touches[i];
        RayHit entry;
        const auto handle = impl_->handlesByActor.find(touch.actor);
        entry.body = handle == impl_->handlesByActor.end()
                         ? BodyHandle::Invalid
                         : static_cast<BodyHandle>(handle->second);
        entry.position = FromPx(touch.position);
        entry.normal = FromPx(touch.normal);
        entry.distance = touch.distance;
        out.push_back(entry);
    }

    // PhysX tidak menjanjikan urutan untuk sentuhan yang dikumpulkan, dan
    // "tembus berapa lapis" hanya bisa dijawab kalau urutannya sepanjang ray.
    std::sort(out.begin(), out.end(),
              [](const RayHit& a, const RayHit& b) { return a.distance < b.distance; });
    return out.size();
}

bool PhysicsWorld::SweepSphere(float radius, const Vec3& origin, const Vec3& direction,
                               float maxDistance, RayHit& out, const QueryFilter& filter) const {
    out = RayHit{};
    SIM_PHYSICS_QUERY_GUARD(false)

    const float length = glm::length(direction);
    if (!(length > 0.0f) || !(maxDistance > 0.0f) || !(radius > 0.0f)) {
        return false;
    }

    physx::PxSweepBuffer hit;
    LayerFilter layerFilter;
    const bool found = impl_->scene->sweep(
        physx::PxSphereGeometry(radius), physx::PxTransform(ToPx(origin)), ToPx(direction / length),
        maxDistance, hit, physx::PxHitFlag::ePOSITION | physx::PxHitFlag::eNORMAL,
        MakeFilterData(filter), &layerFilter);
    if (!found || !hit.hasBlock) {
        return false;
    }

    const auto handle = impl_->handlesByActor.find(hit.block.actor);
    out.body = handle == impl_->handlesByActor.end()
                   ? BodyHandle::Invalid
                   : static_cast<BodyHandle>(handle->second);
    out.position = FromPx(hit.block.position);
    out.normal = FromPx(hit.block.normal);
    out.distance = hit.block.distance;
    return true;
}

std::size_t PhysicsWorld::OverlapSphere(const Vec3& center, float radius,
                                        std::vector<BodyHandle>& out,
                                        const QueryFilter& filter) const {
    out.clear();
    SIM_PHYSICS_QUERY_GUARD(0)

    if (!(radius > 0.0f)) {
        return 0;
    }

    constexpr std::size_t kMaxHits = 256;
    physx::PxOverlapHit buffer[kMaxHits];
    physx::PxOverlapBuffer hits(buffer, static_cast<physx::PxU32>(kMaxHits));

    LayerFilter layerFilter;
    physx::PxQueryFilterData data = MakeFilterData(filter);
    data.flags |= physx::PxQueryFlag::eNO_BLOCK;

    impl_->scene->overlap(physx::PxSphereGeometry(radius), physx::PxTransform(ToPx(center)), hits,
                          data, &layerFilter);

    if (hits.nbTouches >= kMaxHits) {
        SIM_WARN("Physics", "an overlap touched at least {} shapes; the list stops there",
                 kMaxHits);
    }

    // Satu benda bisa punya beberapa bentuk, dan yang bertanya "siapa di radius
    // ledakan" menginginkan daftar benda, bukan daftar bentuk — kalau tidak,
    // sebuah benda menerima kerusakan dua kali karena kebetulan tersusun dari
    // dua bagian.
    out.reserve(hits.nbTouches);
    for (physx::PxU32 i = 0; i < hits.nbTouches; ++i) {
        const auto handle = impl_->handlesByActor.find(hits.touches[i].actor);
        if (handle == impl_->handlesByActor.end()) {
            continue;
        }
        const BodyHandle body = static_cast<BodyHandle>(handle->second);
        if (std::find(out.begin(), out.end(), body) == out.end()) {
            out.push_back(body);
        }
    }
    return out.size();
}

#undef SIM_PHYSICS_QUERY_GUARD

}  // namespace sim::physics

#else  // SIM_WITH_PHYSX

namespace sim::physics {

/// Tanpa PhysX, `Impl` cukup menyimpan pesannya. Bentuk yang sama dipakai kedua
/// cabang supaya header publiknya tidak perlu tahu cabang mana yang aktif.
struct PhysicsWorld::Impl {
    std::string error;
};

bool Available() { return false; }
const char* BackendVersion() { return ""; }
bool GpuAvailable() { return false; }

PhysicsWorld::PhysicsWorld() = default;
PhysicsWorld::~PhysicsWorld() = default;
PhysicsWorld::PhysicsWorld(PhysicsWorld&&) noexcept = default;
PhysicsWorld& PhysicsWorld::operator=(PhysicsWorld&&) noexcept = default;

bool PhysicsWorld::Create(const WorldDesc& /*desc*/) {
    impl_ = std::make_unique<Impl>();
    impl_->error =
        "this build has no PhysX, so nothing is simulated; bodies keep the transform they were "
        "given. See docs/DEPENDENCIES.md.";
    // **Dikatakan, bukan didiamkan.** Benda diam yang seharusnya jatuh terbaca
    // sebagai bug fisika, bukan sebagai pustaka yang tidak dipasang.
    SIM_WARN("Physics", "{}", impl_->error);
    return false;
}

void PhysicsWorld::Destroy() { impl_.reset(); }
bool PhysicsWorld::IsValid() const { return false; }

const std::string& PhysicsWorld::Error() const {
    static const std::string kNone;
    return impl_ != nullptr ? impl_->error : kNone;
}

BodyHandle PhysicsWorld::AddBody(const BodyDesc& /*desc*/) { return BodyHandle::Invalid; }
void PhysicsWorld::RemoveBody(BodyHandle /*body*/) {}
bool PhysicsWorld::IsAlive(BodyHandle /*body*/) const { return false; }
std::size_t PhysicsWorld::BodyCount() const { return 0; }
bool PhysicsWorld::ReadState(BodyHandle /*body*/, BodyState& /*out*/) const { return false; }
bool PhysicsWorld::MoveKinematic(BodyHandle, const Vec3&, const Quat&) { return false; }
bool PhysicsWorld::Teleport(BodyHandle, const Vec3&, const Quat&) { return false; }
uint32_t PhysicsWorld::Advance(float /*deltaSeconds*/) { return 0; }
void PhysicsWorld::Step(uint32_t /*steps*/) {}
float PhysicsWorld::Alpha() const { return 0.0f; }
uint64_t PhysicsWorld::StepCount() const { return 0; }

VehicleHandle PhysicsWorld::AddVehicle(const VehicleDesc&) { return VehicleHandle::Invalid; }
void PhysicsWorld::RemoveVehicle(VehicleHandle) {}
bool PhysicsWorld::IsVehicleAlive(VehicleHandle) const { return false; }
std::size_t PhysicsWorld::VehicleCount() const { return 0; }
bool PhysicsWorld::SetVehicleInput(VehicleHandle, const VehicleInput&) { return false; }

bool PhysicsWorld::ReadVehicleState(VehicleHandle, VehicleState& out) const {
    out = VehicleState{};
    return false;
}

BodyHandle PhysicsWorld::VehicleChassis(VehicleHandle) const { return BodyHandle::Invalid; }

ArticulationHandle PhysicsWorld::AddArticulation(const ArticulationDesc&) {
    return ArticulationHandle::Invalid;
}
void PhysicsWorld::RemoveArticulation(ArticulationHandle) {}
bool PhysicsWorld::IsArticulationAlive(ArticulationHandle) const { return false; }
std::size_t PhysicsWorld::ArticulationCount() const { return 0; }
std::size_t PhysicsWorld::ArticulationLinkCount(ArticulationHandle) const { return 0; }

bool PhysicsWorld::ReadLinkState(ArticulationHandle, std::size_t, BodyState& out) const {
    out = BodyState{};
    return false;
}

JointHandle PhysicsWorld::AddJoint(const JointDesc&) { return JointHandle::Invalid; }
void PhysicsWorld::RemoveJoint(JointHandle) {}
bool PhysicsWorld::IsJointAlive(JointHandle) const { return false; }
std::size_t PhysicsWorld::JointCount() const { return 0; }

bool PhysicsWorld::ReadJointState(JointHandle, JointState& out) const {
    out = JointState{};
    return false;
}

// Query menjawab "tidak kena", bukan menolak dengan galat: adegan tanpa satu
// pun benda memang tidak punya yang bisa kena, dan itulah keadaan build ini.
// Yang memanggilnya sudah diberi tahu lewat `Create` yang gagal.
bool PhysicsWorld::Raycast(const Vec3&, const Vec3&, float, RayHit& out,
                           const QueryFilter&) const {
    out = RayHit{};
    return false;
}

std::size_t PhysicsWorld::RaycastAll(const Vec3&, const Vec3&, float, std::vector<RayHit>& out,
                                     const QueryFilter&) const {
    out.clear();
    return 0;
}

bool PhysicsWorld::SweepSphere(float, const Vec3&, const Vec3&, float, RayHit& out,
                               const QueryFilter&) const {
    out = RayHit{};
    return false;
}

std::size_t PhysicsWorld::OverlapSphere(const Vec3&, float, std::vector<BodyHandle>& out,
                                        const QueryFilter&) const {
    out.clear();
    return 0;
}

}  // namespace sim::physics

#endif  // SIM_WITH_PHYSX
