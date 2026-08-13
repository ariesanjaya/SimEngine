#include "Sim/Physics/PhysicsArticulation.h"

#include <algorithm>
#include <cmath>

// Tanpa satu pun `#if SIM_WITH_PHYSX`, dengan alasan yang sama seperti
// `PhysicsScene.cpp`: yang di sini hanya menyusun deskriptor, dan deskriptor
// tidak butuh solver untuk benar. Ia karena itu dikompilasi dan diuji sama
// persis di kedua build — termasuk seluruh aritmetika penempatan kapsul, yang
// justru bagian yang paling mudah salah.

namespace sim::physics {
namespace {

/// Putaran yang membawa +X ke arah `direction`.
///
/// Kapsul PhysX berbaring di sepanjang sumbu X lokalnya, jadi menempatkan satu
/// di antara dua sendi berarti memutar +X ke arah tulangnya. Ditulis lewat
/// sumbu-sudut alih-alih memakai `glm::quatLookAt`, yang menyejajarkan -Z dan
/// karena itu menghasilkan kapsul yang tegak lurus terhadap tulangnya.
Quat RotationFromXTo(const Vec3& direction) {
    const float length = glm::length(direction);
    if (length < 1e-6f) {
        return Quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    const Vec3 target = direction / length;
    const Vec3 axisX(1.0f, 0.0f, 0.0f);
    const float dot = glm::clamp(glm::dot(axisX, target), -1.0f, 1.0f);

    // Berlawanan arah: sumbu putarnya tidak tertentu lewat cross product, jadi
    // dipilih sumbu tegak lurus mana pun. Tanpa cabang ini hasilnya quaternion
    // ber-NaN, dan NaN di sebuah pose menyebar ke seluruh articulation pada
    // langkah pertama — persis gejala "ragdoll meledak".
    if (dot < -1.0f + 1e-6f) {
        return Quat(0.0f, 0.0f, 0.0f, 1.0f);
    }
    const Vec3 axis = glm::cross(axisX, target);
    return glm::normalize(Quat(1.0f + dot, axis.x, axis.y, axis.z));
}

}  // namespace

ArticulationDesc BuildRagdoll(const std::vector<RagdollBone>& bones,
                              const RagdollSettings& settings,
                              std::vector<std::string>* outSkipped) {
    ArticulationDesc desc;
    if (outSkipped != nullptr) {
        outSkipped->clear();
    }
    if (bones.empty()) {
        return desc;
    }

    // Urutan topologis diperiksa, bukan diasumsikan. Rangka yang melanggarnya
    // menghasilkan articulation yang menunjuk link yang belum dibuat, dan PhysX
    // menjawabnya dengan crash alih-alih dengan pesan.
    for (std::size_t i = 0; i < bones.size(); ++i) {
        const int parent = bones[i].parent;
        if (parent >= static_cast<int>(i)) {
            return ArticulationDesc{};
        }
        if (parent < -1) {
            return ArticulationDesc{};
        }
    }
    if (bones.front().parent != -1) {
        return ArticulationDesc{};
    }

    // Panjang tiap tulang diukur ke anak terjauhnya: itulah ruas yang terlihat
    // sebagai "tulang" di rig, dan yang harus ditutupi kapsulnya.
    std::vector<float> boneLength(bones.size(), 0.0f);
    std::vector<Vec3> boneDirection(bones.size(), Vec3(1.0f, 0.0f, 0.0f));
    for (std::size_t i = 0; i < bones.size(); ++i) {
        const int parent = bones[i].parent;
        if (parent < 0) {
            continue;
        }
        const Vec3 offset = bones[i].position - bones[static_cast<std::size_t>(parent)].position;
        const float length = glm::length(offset);
        if (length > boneLength[static_cast<std::size_t>(parent)]) {
            boneLength[static_cast<std::size_t>(parent)] = length;
            boneDirection[static_cast<std::size_t>(parent)] = offset / std::max(length, 1e-6f);
        }
    }
    // Tulang ujung — tangan, kaki, kepala — tidak punya anak, jadi panjangnya
    // nol menurut ukuran di atas. Mereka tetap butuh bentuk, dan yang dipakai
    // adalah **jaraknya sendiri dari induknya**, bukan panjang induknya.
    //
    // Bedanya justru yang penting: memakai panjang induk ikut menyelamatkan
    // tulang bantu sepanjang nol — yang letaknya persis di induknya — sehingga
    // ia mendapat kapsul kembar yang menempati ruang yang sama. Dua bentuk yang
    // saling menembus sejak langkah nol adalah bentuk paling umum dari ragdoll
    // yang meledak.
    for (std::size_t i = 0; i < bones.size(); ++i) {
        const int parent = bones[i].parent;
        if (boneLength[i] > 0.0f || parent < 0) {
            continue;
        }
        const Vec3 offset = bones[i].position - bones[static_cast<std::size_t>(parent)].position;
        const float length = glm::length(offset);
        if (length > 1e-6f) {
            boneLength[i] = length;
            boneDirection[i] = offset / length;
        }
        // Yang tetap nol jatuh di bawah `minBoneLength` dan akan dilipat.
    }

    // Bone yang terlalu pendek dilipat ke induknya: `linkOf[i]` menunjuk link
    // yang mewakili bone i, yang bisa jadi milik leluhurnya.
    std::vector<int> linkOf(bones.size(), -1);

    for (std::size_t i = 0; i < bones.size(); ++i) {
        const RagdollBone& bone = bones[i];
        const bool isRoot = bone.parent < 0;
        const float length = boneLength[i];

        if (!isRoot && length < settings.minBoneLength) {
            // Dilipat ke induknya, bukan dibuang: yang di bawahnya tetap butuh
            // sesuatu untuk digantungi.
            linkOf[i] = linkOf[static_cast<std::size_t>(bone.parent)];
            if (outSkipped != nullptr) {
                outSkipped->push_back(bone.name);
            }
            continue;
        }

        ArticulationLinkDesc link;
        link.name = bone.name;
        link.parent = isRoot ? kArticulationRootParent
                             : linkOf[static_cast<std::size_t>(bone.parent)];
        // Induk yang dilipat sampai habis membuat link ini menjadi akar kedua,
        // yang tidak sah. Dilekatkan ke akar sebagai gantinya.
        if (!isRoot && link.parent < 0) {
            link.parent = 0;
        }

        const float radius = std::clamp(length * settings.radiusRatio, settings.minRadius,
                                        settings.maxRadius);
        link.shape.kind = ShapeKind::Capsule;
        link.shape.radius = radius;
        // Setengah-tinggi bagian silinder, tanpa tudungnya. Tulang yang lebih
        // pendek daripada dua kali jari-jarinya menjadi bola, bukan kapsul
        // bertinggi negatif — yang ditolak PhysX.
        link.shape.halfExtents.x = std::max(length * 0.5f - radius, 0.0f);

        // Kapsul berbaring di tengah tulang, bukan di pangkalnya: itulah yang
        // membuat bentuknya menutupi daging di antara dua sendi alih-alih
        // menonjol keluar melewati yang berikutnya.
        const Quat along = RotationFromXTo(boneDirection[i]);
        link.position = bone.position + boneDirection[i] * (length * 0.5f);
        link.rotation = along;


        if (!isRoot) {
            link.joint = ArticulationJointKind::Spherical;
            link.limitEnabled = true;
            link.lowerLimit = -settings.swingLimit;
            link.upperLimit = settings.swingLimit;
            link.swing2Limit = settings.swingLimit;

            // Sendi duduk di pangkal tulang ini — yaitu di posisi bone-nya —
            // dinyatakan di ruang lokal masing-masing link.
            const std::size_t parentLink = static_cast<std::size_t>(link.parent);
            const ArticulationLinkDesc& parentDesc = desc.links[parentLink];
            const Quat parentInverse = glm::conjugate(parentDesc.rotation);
            link.parentAnchor = parentInverse * (bone.position - parentDesc.position);
            link.parentFrame = parentInverse * along;
            link.childAnchor = glm::conjugate(along) * (bone.position - link.position);
            link.childFrame = Quat(1.0f, 0.0f, 0.0f, 0.0f);
        }

        linkOf[i] = static_cast<int>(desc.links.size());
        desc.links.push_back(link);
    }

    // **Massa dibagikan sesudah seluruh bentuk diketahui**, menurut volumenya,
    // lalu perbandingannya dibatasi. Dua langkah, dan keduanya perlu:
    //
    // Volume saja memberi proporsi yang masuk akal antar-anggota badan, tetapi
    // bentuk yang disusun otomatis tidak tahu apa-apa tentang anatomi — tulang
    // pinggul yang pendek menghasilkan kapsul mungil sementara paha menghasilkan
    // yang besar, sehingga akar tubuh menjadi puluhan kali lebih ringan daripada
    // anaknya. Articulation dengan perbandingan sebesar itu tidak sekadar
    // bergoyang: ia menghasilkan koordinat NaN, terukur pada langkah ke-55 dengan
    // rig Mixamo sebelum pembatasan ini ada.
    if (!desc.links.empty() && settings.totalMass > 0.0f) {
        std::vector<float> volume(desc.links.size(), 0.0f);
        float totalVolume = 0.0f;
        for (std::size_t i = 0; i < desc.links.size(); ++i) {
            const float r = desc.links[i].shape.radius;
            const float h = desc.links[i].shape.halfExtents.x;
            // Kapsul: silinder ditambah dua belahan bola.
            volume[i] = 3.14159265f * r * r * (2.0f * h) + (4.0f / 3.0f) * 3.14159265f * r * r * r;
            totalVolume += volume[i];
        }

        const float count = static_cast<float>(desc.links.size());
        const float average = settings.totalMass / count;
        const float floorMass = average * settings.minMassRatio;
        const float ceilingMass = average * settings.maxMassRatio;

        float assigned = 0.0f;
        for (std::size_t i = 0; i < desc.links.size(); ++i) {
            const float share =
                totalVolume > 0.0f ? settings.totalMass * (volume[i] / totalVolume) : average;
            desc.links[i].mass = std::clamp(share, floorMass, ceilingMass);
            assigned += desc.links[i].mass;
        }

        // Dinormalkan supaya totalnya tetap `totalMass` sesudah dibatasi —
        // tanpa ini sebuah rig berlengan panjang akan berbobot dua kali lipat
        // rig lain yang massanya diminta sama.
        if (assigned > 0.0f) {
            const float correction = settings.totalMass / assigned;
            for (ArticulationLinkDesc& link : desc.links) {
                link.mass *= correction;
            }
        }
    }

    desc.fixBase = false;
    desc.jointFriction = settings.jointFriction;
    return desc;
}

}  // namespace sim::physics
