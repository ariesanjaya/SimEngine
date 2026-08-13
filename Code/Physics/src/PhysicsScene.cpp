#include "Sim/Physics/PhysicsScene.h"

#include "Sim/Core/Log.h"
#include "Sim/Scene/Components.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

// **Tidak ada satu pun `#if SIM_WITH_PHYSX` di berkas ini, dan itu bukan
// kelalaian.** Jembatannya hanya memakai API publik `PhysicsWorld`, yang sudah
// menangani ketiadaan PhysX dengan menolak secara terbuka. Hasilnya berkas ini
// dikompilasi dan diuji sama persis di kedua build — dan kelas cacat yang paling
// sering lolos dari dependensi opsional, yaitu jalur yang hanya pernah
// dikompilasi di satu konfigurasi, tidak punya tempat untuk bersembunyi.

namespace sim::physics {
namespace {

/// Memisahkan matriks dunia menjadi posisi, putaran, dan skala.
///
/// Ditulis tangan alih-alih memakai `glm::decompose`: yang terakhir juga
/// mengembalikan skew dan perspektif yang tidak pernah dipakai di sini, dan
/// gagal secara diam-diam untuk matriks yang skalanya nol pada satu sumbu.
struct Decomposed {
    Vec3 position{0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f};
};

Decomposed Decompose(const Mat4& matrix) {
    Decomposed out;
    out.position = Vec3(matrix[3]);

    Vec3 axes[3] = {Vec3(matrix[0]), Vec3(matrix[1]), Vec3(matrix[2])};
    for (int i = 0; i < 3; ++i) {
        const float length = glm::length(axes[i]);
        out.scale[i] = length;
        // Sumbu berskala nol tidak bisa dinormalkan. Diganti sumbu satuan supaya
        // yang keluar tetap putaran yang sah: entity berskala nol tidak terlihat,
        // tetapi ia tidak boleh menghasilkan quaternion NaN yang lalu menyebar
        // ke seluruh solver.
        axes[i] = length > 1e-8f ? axes[i] / length : Vec3(i == 0, i == 1, i == 2);
    }

    // Skala negatif (mirror) terbaca sebagai basis kidal. Quaternion tidak bisa
    // menyatakannya, jadi tandanya dipindahkan ke skala dan putarannya dibuat
    // kanan — bentuk tabrakan yang dicerminkan memang tidak ada di PhysX.
    if (glm::dot(glm::cross(axes[0], axes[1]), axes[2]) < 0.0f) {
        axes[0] = -axes[0];
        out.scale.x = -out.scale.x;
    }

    out.rotation = glm::quat_cast(Mat3(axes[0], axes[1], axes[2]));
    return out;
}

/// Skala seragam yang dipakai bentuk yang tidak bisa diskalakan per-sumbu.
///
/// Diambil yang terbesar, bukan rata-ratanya: bola yang mengecil di bawah
/// bentuk yang digambar membuat benda tampak melayang, sedangkan yang sedikit
/// membesar hanya membuatnya berhenti sedikit lebih awal — kesalahan yang jauh
/// lebih mudah dilihat dan jauh lebih tidak merusak.
float UniformScale(const Vec3& scale) {
    return std::max({std::abs(scale.x), std::abs(scale.y), std::abs(scale.z)});
}

bool IsNonUniform(const Vec3& scale) {
    const float largest = UniformScale(scale);
    const float smallest =
        std::min({std::abs(scale.x), std::abs(scale.y), std::abs(scale.z)});
    return largest > 1e-6f && (largest - smallest) / largest > 1e-3f;
}

/// Menyusun empat roda dari jarak sumbu dan jarak jejak.
///
/// **Diturunkan, bukan diketik satu per satu.** Jarak sumbu dan jarak jejak
/// adalah dua angka yang dipikirkan orang saat merancang mobil; empat koordinat
/// lepas adalah empat kesempatan menaruh roda tidak simetris tanpa menyadarinya.
std::vector<VehicleWheelDesc> WheelsFrom(const scene::VehicleComponent& vehicle) {
    const float halfBase = vehicle.wheelbase * 0.5f;
    const float halfTrack = vehicle.trackWidth * 0.5f;

    std::vector<VehicleWheelDesc> wheels;
    wheels.reserve(4);
    // Urutannya: depan-kiri, depan-kanan, belakang-kiri, belakang-kanan.
    for (int i = 0; i < 4; ++i) {
        const bool isFront = i < 2;
        const bool isLeft = (i % 2) == 0;

        VehicleWheelDesc wheel;
        wheel.centerOffset = Vec3(isLeft ? halfTrack : -halfTrack, vehicle.axleHeight,
                                  isFront ? halfBase : -halfBase);
        wheel.radius = vehicle.wheelRadius;
        wheel.width = vehicle.wheelWidth;
        wheel.mass = vehicle.wheelMass;
        wheel.suspensionTravel = vehicle.suspensionTravel;
        wheel.steered = isFront;
        wheel.driven = vehicle.drive == scene::VehicleDriveKind::AllWheel ||
                       (vehicle.drive == scene::VehicleDriveKind::FrontWheel) == isFront;
        // Rem tangan mengunci roda belakang — itu yang membuat mobil berputar
        // alih-alih berhenti lurus, dan itulah gunanya.
        wheel.handbraked = !isFront;
        wheels.push_back(wheel);
    }
    return wheels;
}

JointKind ToJointKind(scene::JointType type) {
    switch (type) {
        case scene::JointType::Fixed: return JointKind::Fixed;
        case scene::JointType::Revolute: return JointKind::Revolute;
        case scene::JointType::Prismatic: return JointKind::Prismatic;
        case scene::JointType::Spherical: return JointKind::Spherical;
        case scene::JointType::D6: return JointKind::D6;
    }
    return JointKind::Fixed;
}

BodyKind ToBodyKind(scene::RigidBodyKind kind) {
    switch (kind) {
        case scene::RigidBodyKind::Static: return BodyKind::Static;
        case scene::RigidBodyKind::Kinematic: return BodyKind::Kinematic;
        case scene::RigidBodyKind::Dynamic: return BodyKind::Dynamic;
    }
    return BodyKind::Dynamic;
}

ShapeKind ToShapeKind(scene::ColliderShape shape) {
    switch (shape) {
        case scene::ColliderShape::Box: return ShapeKind::Box;
        case scene::ColliderShape::Sphere: return ShapeKind::Sphere;
        case scene::ColliderShape::Capsule: return ShapeKind::Capsule;
        case scene::ColliderShape::Plane: return ShapeKind::Plane;
        case scene::ColliderShape::Cylinder: return ShapeKind::Cylinder;
        // Whitebox tidak punya padanan langsung: bentuknya ditentukan oleh
        // benda yang memakainya — segitiga bila diam, selubung bila bergerak —
        // jadi yang memutuskannya adalah `Build`, bukan pemetaan ini.
        case scene::ColliderShape::Whitebox: return ShapeKind::ConvexHull;
    }
    return ShapeKind::Box;
}

/// Menerjemahkan collider beserta skala entity menjadi bentuk solver.
ShapeDesc ToShapeDesc(const scene::ColliderComponent& collider, const Vec3& scale) {
    ShapeDesc shape;
    shape.kind = ToShapeKind(collider.shape);
    shape.localPosition = collider.offset * scale;

    const float uniform = UniformScale(scale);
    switch (shape.kind) {
        case ShapeKind::Box:
            shape.halfExtents = collider.halfExtents * glm::abs(scale);
            break;
        case ShapeKind::Sphere:
            shape.radius = collider.radius * uniform;
            break;
        case ShapeKind::Capsule:
            shape.radius = collider.radius * uniform;
            // `ShapeDesc` menyimpan setengah-tinggi silinder kapsul di
            // `halfExtents.x` — konvensi PhysX, diterjemahkan di sini supaya
            // `ColliderComponent` bisa menyebutnya dengan namanya sendiri.
            shape.halfExtents.x = collider.halfHeight * uniform;
            break;
        case ShapeKind::Cylinder:
            shape.radius = collider.radius * uniform;
            shape.halfExtents.x = collider.halfHeight * uniform;
            break;
        case ShapeKind::Plane:
        case ShapeKind::ConvexHull:
        case ShapeKind::TriangleMesh:
            // Titik-titiknya diisi `Build`, yang punya sumber bentuknya.
            break;
    }
    return shape;
}

/// Menyalin bentuk dari aset ke dalam `ShapeDesc`, sudah berskala.
///
/// **Skalanya diterapkan pada titiknya, bukan disimpan di sebelahnya.** Itu yang
/// membuat whitebox boleh diskalakan tidak seragam sementara bola dan kapsul
/// tidak: sebuah titik bisa diregangkan ke satu arah, sebuah jari-jari tidak.
void ApplyGeometry(ShapeDesc& shape, const ColliderGeometry& geometry, const Vec3& scale,
                   bool asTriangleMesh) {
    shape.kind = asTriangleMesh ? ShapeKind::TriangleMesh : ShapeKind::ConvexHull;
    shape.points.clear();
    shape.points.reserve(geometry.points.size());
    for (const Vec3& point : geometry.points) {
        shape.points.push_back(point * scale);
    }
    shape.indices = asTriangleMesh ? geometry.indices : std::vector<uint32_t>{};
}

/// Menelusuri hierarki dari akar, induk sebelum anak.
///
/// **Urutan ini bagian dari masukan simulasi, bukan detail.** PhysX menyelesaikan
/// kontak dalam urutan benda dimasukkan, jadi dua muat level yang sama harus
/// menghasilkan urutan yang sama. Penelusuran hierarki memberikannya karena
/// urutannya urutan berkas; iterasi view entt memberikannya juga hari ini, tetapi
/// lewat urutan kolam komponen yang tidak dijanjikan API-nya.
void CollectDepthFirst(const scene::World& world, scene::Entity entity,
                       std::vector<scene::Entity>& out) {
    out.push_back(entity);
    for (const scene::Entity child : world.ChildrenOf(entity)) {
        CollectDepthFirst(world, child, out);
    }
}

}  // namespace

bool PhysicsScene::Build(scene::World& world, const WorldDesc& desc,
                         const ColliderGeometrySource& geometry) {
    Clear();

    if (!simulation_.Create(desc)) {
        return false;
    }

    std::vector<scene::Entity> order;
    order.reserve(world.Count());
    for (const scene::Entity root : world.Roots()) {
        CollectDepthFirst(world, root, order);
    }

    for (const scene::Entity entity : order) {
        const auto* body = world.TryGet<scene::RigidBodyComponent>(entity);
        if (body == nullptr) {
            continue;
        }
        const auto* collider = world.TryGet<scene::ColliderComponent>(entity);
        if (collider == nullptr) {
            // Dilaporkan sekali per entity, dengan namanya. Benda tanpa bentuk
            // jatuh menembus lantai, dan tanpa baris ini satu-satunya petunjuk
            // adalah entity yang menghilang dari layar.
            ++stats_.skippedWithoutCollider;
            SIM_WARN("Physics", "'{}' has a Rigid Body but no Collider, so it is not simulated",
                     world.NameOf(entity));
            continue;
        }

        const Decomposed placement = Decompose(world.WorldMatrix(entity));

        const bool needsUniform = collider->shape == scene::ColliderShape::Sphere ||
                                  collider->shape == scene::ColliderShape::Capsule ||
                                  collider->shape == scene::ColliderShape::Cylinder;
        if (needsUniform && IsNonUniform(placement.scale)) {
            ++stats_.nonUniformScale;
            SIM_WARN("Physics",
                     "'{}' is scaled unevenly but its collider cannot be, so the largest axis is "
                     "used and the shape will not match what is drawn",
                     world.NameOf(entity));
        }

        BodyDesc bodyDesc;
        bodyDesc.kind = ToBodyKind(body->kind);
        bodyDesc.shape = ToShapeDesc(*collider, placement.scale);

        if (collider->shape == scene::ColliderShape::Whitebox) {
            ColliderGeometry shape;
            if (geometry && geometry(entity, shape) && shape.points.size() >= 4) {
                // Mesh segitiga hanya untuk yang tidak bergerak. Yang bergerak
                // memakai selubungnya, dan bila bentuknya cekung selubung itu
                // memang berbeda dari yang digambar — dikatakan, bukan
                // didiamkan.
                const bool movable = bodyDesc.kind == BodyKind::Dynamic;
                const bool asTriangleMesh = !movable && !shape.indices.empty();
                if (movable && !shape.convex) {
                    ++stats_.concaveDynamic;
                    SIM_WARN("Physics",
                             "'{}' is a concave whitebox on a dynamic body, so its convex hull is "
                             "used and the hollows are filled in",
                             world.NameOf(entity));
                }
                ApplyGeometry(bodyDesc.shape, shape, placement.scale, asTriangleMesh);
            } else {
                // Mundur ke kotak, bukan melewatkan bendanya. Benda yang hilang
                // dari simulasi terlihat sebagai benda yang jatuh menembus
                // lantai; kotak yang salah ukuran terlihat sebagai kotak yang
                // salah ukuran, dan itu bisa dilacak.
                ++stats_.collidersWithoutGeometry;
                SIM_WARN("Physics",
                         "'{}' asks for a Whitebox collider but no shape could be read, so a box "
                         "is used instead",
                         world.NameOf(entity));
                bodyDesc.shape.kind = ShapeKind::Box;
                bodyDesc.shape.halfExtents = collider->halfExtents * glm::abs(placement.scale);
            }
        }

        bodyDesc.material.staticFriction = collider->staticFriction;
        bodyDesc.material.dynamicFriction = collider->dynamicFriction;
        bodyDesc.material.restitution = collider->restitution;
        bodyDesc.position = placement.position;
        bodyDesc.rotation = placement.rotation;
        bodyDesc.mass = body->mass;
        bodyDesc.density = body->density;
        bodyDesc.linearDamping = body->linearDamping;
        bodyDesc.angularDamping = body->angularDamping;
        bodyDesc.allowSleeping = body->allowSleeping;

        const BodyHandle handle = simulation_.AddBody(bodyDesc);
        if (handle == BodyHandle::Invalid) {
            SIM_WARN("Physics", "'{}' could not be added to the simulation",
                     world.NameOf(entity));
            continue;
        }

        Tracked entry;
        entry.entity = entity;
        entry.body = handle;
        entry.scale = placement.scale;
        entry.kinematic = body->kind == scene::RigidBodyKind::Kinematic;
        entry.dynamic = body->kind == scene::RigidBodyKind::Dynamic;
        tracked_.push_back(entry);
        byEntity_.emplace(static_cast<uint32_t>(entity), handle);
        ++stats_.bodies;
    }

    // Kendaraan disusun sesudah benda biasa dan sebelum sendi: sendi boleh
    // menunjuk chassis sebuah kendaraan, jadi chassis-nya harus sudah terdaftar.
    for (const scene::Entity entity : order) {
        const auto* vehicleComponent = world.TryGet<scene::VehicleComponent>(entity);
        if (vehicleComponent == nullptr) {
            continue;
        }
        if (world.Has<scene::RigidBodyComponent>(entity)) {
            // Bukan penolakan: kendaraannya tetap dibangun, karena itu yang
            // jelas diminta entity ini. Yang dilaporkan adalah bahwa benda tegar
            // keduanya diabaikan — dua benda di tempat yang sama akan saling
            // mendorong dengan cara yang tidak bisa dijelaskan siapa pun.
            ++stats_.vehiclesWithRigidBody;
            SIM_WARN("Physics",
                     "'{}' has both a Vehicle and a Rigid Body; the Rigid Body is ignored",
                     world.NameOf(entity));
        }

        const Decomposed placement = Decompose(world.WorldMatrix(entity));

        // Bukan `desc`: nama itu sudah dipakai parameter `WorldDesc` fungsi ini.
        VehicleDesc vehicleDesc;
        vehicleDesc.chassisHalfExtents = vehicleComponent->chassisHalfExtents * glm::abs(placement.scale);
        vehicleDesc.chassisMass = vehicleComponent->chassisMass;
        vehicleDesc.centerOfMassOffset = vehicleComponent->centerOfMassOffset;
        vehicleDesc.wheels = WheelsFrom(*vehicleComponent);
        vehicleDesc.maxSteerAngle = vehicleComponent->maxSteerAngle;
        vehicleDesc.peakDriveTorque = vehicleComponent->peakDriveTorque;
        vehicleDesc.maxBrakeTorque = vehicleComponent->maxBrakeTorque;
        vehicleDesc.maxHandbrakeTorque = vehicleComponent->maxHandbrakeTorque;
        vehicleDesc.tireFriction = vehicleComponent->tireFriction;
        vehicleDesc.position = placement.position;
        vehicleDesc.rotation = placement.rotation;

        const VehicleHandle handle = simulation_.AddVehicle(vehicleDesc);
        if (handle == VehicleHandle::Invalid) {
            SIM_WARN("Physics", "the Vehicle on '{}' could not be created: {}",
                     world.NameOf(entity), simulation_.Error());
            continue;
        }
        ++stats_.vehicles;
        vehiclesByEntity_.emplace(static_cast<uint32_t>(entity), handle);

        // Chassis-nya ikut dilacak seperti benda dinamis, sehingga transform-nya
        // ditulis balik ke scene tiap langkah tanpa jalur terpisah.
        Tracked entry;
        entry.entity = entity;
        entry.body = simulation_.VehicleChassis(handle);
        entry.scale = placement.scale;
        entry.dynamic = true;
        tracked_.push_back(entry);
        if (entry.body != BodyHandle::Invalid) {
            byEntity_.emplace(static_cast<uint32_t>(entity), entry.body);
        }
    }

    // **Sendi dibangun sesudah seluruh benda ada**, dalam sapuan kedua. Sendi
    // bisa menunjuk entity mana pun di level, termasuk yang tersusun di bawahnya
    // dalam hierarki — membangunnya di sapuan yang sama akan membuat sendi
    // berhasil atau gagal tergantung urutan entity di berkas, yang bukan sesuatu
    // yang pernah dipikirkan orang saat menyusun levelnya.
    for (const scene::Entity entity : order) {
        const auto* joint = world.TryGet<scene::JointComponent>(entity);
        if (joint == nullptr) {
            continue;
        }

        const BodyHandle self = BodyOf(entity);
        if (self == BodyHandle::Invalid) {
            ++stats_.skippedJoints;
            SIM_WARN("Physics", "'{}' has a Joint but is not a physics body itself",
                     world.NameOf(entity));
            continue;
        }

        // GUID kosong berarti dunia — sah, dan itulah cara pintu dan bandul
        // dipasang. GUID yang terisi tapi tidak ditemukan adalah hal lain: sendi
        // yang menunjuk entity yang sudah dihapus, dan itu harus terdengar.
        BodyHandle other = BodyHandle::Invalid;
        if (joint->connectedBody != Uuid{}) {
            const scene::Entity target = world.FindByGuid(joint->connectedBody);
            other = target == scene::kNullEntity ? BodyHandle::Invalid : BodyOf(target);
            if (other == BodyHandle::Invalid) {
                ++stats_.skippedJoints;
                SIM_WARN("Physics",
                         "the Joint on '{}' points at a body that is not in this level",
                         world.NameOf(entity));
                continue;
            }
        }

        // Bukan `desc`: nama itu sudah dipakai `WorldDesc` parameter fungsi ini,
        // dan bayangan nama seperti itu adalah cara paling mudah menyunting
        // struct yang salah tanpa satu pun pesan.
        JointDesc jointDesc;
        jointDesc.kind = ToJointKind(joint->type);
        // `bodyB` adalah entity yang membawa komponennya — yang bergerak — dan
        // `bodyA` yang ditunjuknya. Urutan itu yang membuat sudut revolute
        // bertambah ke arah yang sama dengan putaran entity-nya sendiri.
        jointDesc.bodyA = other;
        jointDesc.bodyB = self;
        jointDesc.localAnchorB = joint->anchor;
        jointDesc.localRotationB = joint->frame;
        jointDesc.localRotationA = joint->frame;

        // Bingkai pada ujung satunya harus menunjuk titik dunia yang sama, kalau
        // tidak sendinya lahir dalam keadaan tertarik dan menyentak pada langkah
        // pertama.
        const Mat4 selfWorld = world.WorldMatrix(entity);
        const Vec3 worldAnchor = Vec3(selfWorld * Vec4(joint->anchor, 1.0f));
        if (other == BodyHandle::Invalid) {
            jointDesc.localAnchorA = worldAnchor;
        } else {
            const scene::Entity target = world.FindByGuid(joint->connectedBody);
            const Mat4 otherWorld = world.WorldMatrix(target);
            jointDesc.localAnchorA = Vec3(glm::affineInverse(otherWorld) * Vec4(worldAnchor, 1.0f));
        }

        jointDesc.limit.enabled = joint->limitEnabled;
        jointDesc.limit.lower = joint->lowerLimit;
        jointDesc.limit.upper = joint->upperLimit;
        jointDesc.collisionEnabled = joint->collisionEnabled;
        jointDesc.breakForce = joint->breakForce;
        jointDesc.breakTorque = joint->breakTorque;

        const JointHandle handle = simulation_.AddJoint(jointDesc);
        if (handle == JointHandle::Invalid) {
            ++stats_.skippedJoints;
            SIM_WARN("Physics", "the Joint on '{}' could not be created: {}",
                     world.NameOf(entity), simulation_.Error());
            continue;
        }
        ++stats_.joints;
    }

    return true;
}

void PhysicsScene::Clear() {
    simulation_.Destroy();
    tracked_.clear();
    byEntity_.clear();
    vehiclesByEntity_.clear();
    stats_ = PhysicsSceneStats{};
}

VehicleHandle PhysicsScene::VehicleOf(scene::Entity entity) const {
    const auto it = vehiclesByEntity_.find(static_cast<uint32_t>(entity));
    return it == vehiclesByEntity_.end() ? VehicleHandle::Invalid : it->second;
}

BodyHandle PhysicsScene::BodyOf(scene::Entity entity) const {
    const auto it = byEntity_.find(static_cast<uint32_t>(entity));
    return it == byEntity_.end() ? BodyHandle::Invalid : it->second;
}

uint32_t PhysicsScene::Advance(scene::World& world, float deltaSeconds) {
    if (!simulation_.IsValid()) {
        return 0;
    }
    PushKinematic(world);
    const uint32_t steps = simulation_.Advance(deltaSeconds);
    // **Tidak disalin balik bila tidak ada langkah yang berjalan**, dan itu
    // sering terjadi: pada layar 144 Hz kebanyakan frame tidak memuat satu pun
    // langkah 60 Hz. Menyalinnya tetap berarti menulis angka yang sama persis
    // lalu menandai transform seluruh keturunan setiap entity dinamis perlu
    // dihitung ulang — kerja yang hasilnya identik dengan tidak melakukannya.
    //
    // Yang membuat gerakan tetap mulus di antara langkah adalah interpolasi
    // memakai `PhysicsWorld::Alpha()`, dan itu belum dipasang di sini.
    if (steps > 0) {
        PullDynamic(world);
    }
    return steps;
}

void PhysicsScene::Step(scene::World& world, uint32_t steps) {
    if (!simulation_.IsValid()) {
        return;
    }
    PushKinematic(world);
    simulation_.Step(steps);
    PullDynamic(world);
}

void PhysicsScene::PushKinematic(scene::World& world) {
    for (const Tracked& entry : tracked_) {
        if (!entry.kinematic || !world.IsAlive(entry.entity)) {
            continue;
        }
        // **Arah scene → fisika, dan hanya untuk yang kinematik.** Itulah arti
        // "digerakkan transform, bukan gaya": yang menentukan letaknya adalah
        // apa yang ditulis animasi atau skrip ke `TransformComponent`, dan
        // solver hanya diberi tahu ke mana ia bergerak supaya benda dinamis di
        // jalurnya ikut terdorong.
        const Decomposed placement = Decompose(world.WorldMatrix(entry.entity));
        simulation_.MoveKinematic(entry.body, placement.position, placement.rotation);
    }
}

void PhysicsScene::PullDynamic(scene::World& world) {
    for (const Tracked& entry : tracked_) {
        if (!entry.dynamic || !world.IsAlive(entry.entity)) {
            continue;
        }
        BodyState state;
        if (!simulation_.ReadState(entry.body, state)) {
            continue;
        }
        auto* transform = world.TryGet<scene::TransformComponent>(entry.entity);
        if (transform == nullptr) {
            continue;
        }

        // Solver bekerja di ruang dunia; `TransformComponent` menyimpan ruang
        // induk. Untuk entity berinduk keduanya berbeda, dan menuliskan posisi
        // dunia apa adanya membuat benda melompat sejauh transform induknya —
        // gejala yang muncul hanya pada level yang entity fisikanya ditata di
        // dalam grup, yaitu hampir semua level sungguhan.
        const scene::Entity parent = world.ParentOf(entry.entity);
        if (parent == scene::kNullEntity) {
            transform->position = state.position;
            transform->rotation = state.rotation;
        } else {
            const Mat4 parentWorld = world.WorldMatrix(parent);
            const Mat4 local = glm::affineInverse(parentWorld) *
                               glm::translate(Mat4(1.0f), state.position) *
                               glm::mat4_cast(state.rotation);
            const Decomposed placement = Decompose(local);
            transform->position = placement.position;
            transform->rotation = placement.rotation;
        }

        // Skala sengaja tidak disentuh: solver tidak pernah mengubahnya, dan
        // menuliskannya kembali dari matriks hasil dekomposisi hanya akan
        // menumpuk galat pembulatan sampai benda perlahan mengecil.
        world.MarkTransformDirty(entry.entity);
    }
}

}  // namespace sim::physics
