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

#include <algorithm>
#include <atomic>
#include <unordered_map>

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

    void Shutdown() {
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
