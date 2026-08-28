#include "Sim/SceneView/PickScene.h"

#include "Sim/SceneView/Selection.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <utility>

namespace sim::view {

uint64_t PickScene::Fingerprint(std::span<const PickItem> items) {
    // Menyidik apa yang benar-benar menentukan bentuk scene: entity mana, di
    // mana, dan geometri apa. Warna dan seleksi sengaja tidak ikut — mengubah
    // warna sebuah benda tidak memindahkan satu pun segitiganya.
    //
    // **Per kata, bukan per byte, dan itu bukan mikro-optimisasi.** Sepuluh ribu
    // benda berarti 730 KB yang harus dilewati di dalam setiap klik; FNV-1a
    // byte demi byte atas itu memakan 1,2 ms sendirian — lebih mahal daripada
    // penelusuran yang seharusnya ia hemat, dan cukup untuk melanggar kriteria
    // satu milidetik yang menjadi alasannya ada. Diukur, bukan diduga.
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](uint64_t word) {
        hash ^= word;
        hash *= 1099511628211ull;
    };

    for (const PickItem& item : items) {
        mix(static_cast<uint64_t>(static_cast<uint32_t>(item.entity)));

        // Matriksnya 64 byte yang sejajar delapan — dilewati sebagai delapan
        // kata, bukan enam puluh empat byte.
        uint64_t words[8];
        std::memcpy(words, &item.worldMatrix, sizeof(words));
        for (const uint64_t word : words) {
            mix(word);
        }

        // Kuncinya lewat `std::hash`, yang membaca kata sekaligus. Panjangnya
        // ikut supaya dua kunci berbeda yang hash-nya bertabrakan tetap
        // terbedakan pada kasus yang paling mungkin — nama berkas seukuran sama.
        mix(std::hash<std::string_view>{}(item.meshKey));
        mix(item.meshKey.size());
    }
    return hash;
}

void PickScene::Sync(std::span<const PickItem> items) {
    const uint64_t fingerprint = Fingerprint(items);
    // **Dilewati bila tidak ada yang berubah**, dan itulah yang membuat picking
    // di adegan besar tetap di bawah satu milidetik: kamera yang berputar tidak
    // menggeser satu pun benda, jadi pohon yang sudah ada masih menjawab benar.
    //
    // Kecuali masih ada yang menunggu geometrinya: pemuatan yang selesai di
    // thread lain mengubah isi scene tanpa mengubah daftarnya sama sekali.
    if (synced_ && fingerprint == fingerprint_ && pendingCount_ == 0) {
        return;
    }
    fingerprint_ = fingerprint;
    synced_ = true;

    readyCount_ = 0;
    pendingCount_ = 0;
    loadingCount_ = 0;
    covered_.clear();

    // **Instance dibuang, geometri dipertahankan.** BVH sebuah mesh tidak
    // berubah ketika bendanya bergeser, dan membangunnya ulang di sini akan
    // membuat memindahkan satu kursi membayar seluruh Sponza.
    scene_.ClearInstances();

    for (const PickItem& item : items) {
        if (item.meshKey.empty() || cache_ == nullptr) {
            continue;
        }
        const std::string key(item.meshKey);

        auto found = geometries_.find(key);
        if (found == geometries_.end()) {
            // Belum pernah diminta. Yang punya berkas dimuat asinkron; yang
            // tidak — whitebox, ubin terrain — hanya bisa datang lewat `Adopt`
            // di cache-nya, jadi di sini ia sekadar belum siap.
            const assets::MeshGeometryRef ref =
                item.sourcePath.empty()
                    ? cache_->Find(key)
                    : cache_->Request(std::filesystem::path(item.sourcePath));
            if (ref.state != assets::MeshGeometryState::Ready || ref.data == nullptr) {
                ++pendingCount_;
                // **Yang masih dimuat dibedakan dari yang gagal**, dan itu bukan
                // kerapian: panggangan cahaya harus menunggu yang pertama dan
                // tidak boleh menunggu yang kedua. Berkas yang tidak bisa diurai
                // tidak akan pernah siap, dan menunggunya berarti tombol Bake
                // yang tidak pernah bisa ditekan.
                if (ref.state == assets::MeshGeometryState::Pending) {
                    ++loadingCount_;
                }
                continue;
            }

            const assets::MeshData& mesh = *ref.data;
            // Posisi dibaca langsung dari struct vertex interleaved — offset nol,
            // stride 32. Tidak ada repack, dan tidak ada salinan kedua.
            const raycast::GeometryId id =
                scene_.AddMesh(&mesh.vertices[0].position, sizeof(assets::MeshVertex),
                               mesh.vertices.size(), mesh.indices);
            if (id == raycast::GeometryId::Invalid) {
                ++pendingCount_;
                continue;
            }
            found = geometries_.emplace(key, Geometry{id, ref.data}).first;
        }

        const uint64_t selection = ToSelectionId(item.entity);
        scene_.AddInstance(found->second.id, item.worldMatrix, selection);
        covered_.insert(selection);
        ++readyCount_;
    }

    scene_.Commit();
}

raycast::RayHit PickScene::Raycast(const Vec3& origin, const Vec3& direction,
                                   float maxDistance) const {
    return raycast::Raycast(scene_, origin, direction, maxDistance);
}

raycast::RayHit PickScene::RaycastExcluding(const Vec3& origin, const Vec3& direction,
                                            float maxDistance,
                                            std::span<const scene::Entity> ignore) const {
    // Delapan: cukup untuk benda yang berdiri di dalam beberapa lapis geometri
    // miliknya sendiri, dan tetap terbatas. Yang melewatinya menjawab "tidak
    // kena", bukan berputar selamanya.
    constexpr int kMaxRetries = 8;

    const float length = glm::length(direction);
    if (!(length > 0.0f)) {
        return raycast::RayHit{};
    }
    const Vec3 ray = direction / length;

    Vec3 from = origin;
    float remaining = maxDistance;
    for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
        const raycast::RayHit hit = raycast::Raycast(scene_, from, ray, remaining);
        if (!hit) {
            return hit;
        }
        const scene::Entity entity = ToEntity(hit.userData);
        if (std::find(ignore.begin(), ignore.end(), entity) == ignore.end()) {
            return hit;
        }
        // Maju sedikit melewati permukaan yang barusan dikenai. Tanpa dorongan
        // ini, sinar berikutnya berangkat tepat di bidangnya dan mengenai
        // permukaan yang sama lagi.
        constexpr float kNudge = 1e-4f;
        from = hit.position + ray * kNudge;
        remaining -= hit.distance + kNudge;
        if (!(remaining > 0.0f)) {
            return raycast::RayHit{};
        }
    }
    return raycast::RayHit{};
}

scene::Entity PickScene::RaycastEntity(const Vec3& origin, const Vec3& direction,
                                       float maxDistance) const {
    const raycast::RayHit hit = Raycast(origin, direction, maxDistance);
    // `userData` menyimpan SelectionId, bukan Entity mentah: nol di sana sudah
    // berarti "tidak ada", jadi entity pertama entt tidak tertukar dengan
    // ketiadaan. Lihat `ToSelectionId`.
    return hit ? ToEntity(hit.userData) : scene::kNullEntity;
}

bool PickScene::Covers(scene::Entity entity) const {
    return covered_.count(ToSelectionId(entity)) != 0;
}

void PickScene::Invalidate(const std::string& key) {
    const auto found = geometries_.find(key);
    if (found == geometries_.end()) {
        return;
    }
    geometries_.erase(found);
    // Sidik jarinya dibatalkan juga: daftarnya belum tentu berubah, dan tanpa
    // ini `Sync` berikutnya akan melewatkan penyusunan yang justru diminta.
    synced_ = false;
    // BVH-nya masih ada di dalam `scene_`, tetapi tidak ada lagi yang
    // menunjuknya — `Sync` berikutnya membangunnya kembali dari geometri yang
    // baru. Membuang seluruh scene di sini akan membangun ulang setiap mesh lain
    // hanya karena satu whitebox disunting.
    if (cache_ != nullptr) {
        cache_->Invalidate(key);
    }
}

}  // namespace sim::view
