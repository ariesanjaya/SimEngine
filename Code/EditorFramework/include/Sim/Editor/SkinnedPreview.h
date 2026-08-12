#pragma once

#include "Sim/Animation/Clip.h"
#include "Sim/Animation/Pose.h"
#include "Sim/Animation/Skeleton.h"
#include "Sim/Core/Math.h"
#include "Sim/Scene/World.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace sim::assets {
class AssetDatabase;
}

namespace sim::editor {

/// Memutar klip animasi pada entity yang aset mesh-nya ber-rig, lalu menyediakan
/// palet matriks kulitnya untuk `SceneView`.
///
/// **Di editor, bukan di runtime scene.** Yang menjalankan animasi di dalam game
/// kelak adalah sistem tersendiri beserta graph-nya; yang dibutuhkan sekarang
/// adalah melihat klip yang diimpor bergerak pada mesh yang sudah di-skin — dan
/// itu pekerjaan editor, sama seperti pratinjau material dan thumbnail.
///
/// **Rangkanya datang dari aset mesh, bukan dari rujukan tersendiri.** Indeks
/// bone di dalam bobot skin yang sudah ada di GPU menunjuk ke urutan yang
/// dihasilkan importir mesh; rangka dari sumber lain akan menghasilkan pose yang
/// benar untuk tulang yang salah, dan yang terlihat adalah kulit yang
/// terpelintir — bukan galat.
class SkinnedPreview {
public:
    /// Memajukan waktu tiap animator lalu menghitung ulang paletnya. Dipanggil
    /// sekali per frame, sebelum `SceneView::Build`.
    void Update(scene::World& world, const assets::AssetDatabase* assets, float deltaSeconds);

    /// Palet kulit sebuah entity, atau span kosong kalau ia tidak sedang
    /// beranimasi. Sah sampai `Update` berikutnya.
    std::span<const Mat4> PaletteFor(scene::Entity entity) const;

    /// Membuang seluruh cache. Dipakai saat proyek atau level berganti — jalur
    /// aset yang sama di proyek lain adalah berkas yang lain.
    void Clear();

    /// Berapa entity yang benar-benar sedang dianimasikan frame ini. Dipakai
    /// panel statistik, dan dipakai uji.
    int AnimatedCount() const { return animated_; }

private:
    /// Rangka sebuah berkas mesh, atau null. Jalur yang gagal dicatat gagal dan
    /// tidak dicoba lagi — mengurai berkas rusak enam puluh kali per detik
    /// adalah editor yang berhenti merespons, bukan pesan galat.
    const animation::Skeleton* AcquireSkeleton(const std::filesystem::path& path);
    /// Klip sebuah berkas `.simanim` atau `.fbx`, atau null. Di-cache dengan
    /// alasan yang sama.
    const animation::Clip* AcquireClip(const std::filesystem::path& path);

    /// Keadaan pemutaran satu entity.
    ///
    /// `binding` disimpan, bukan dihitung ulang tiap frame: memasang track ke
    /// bone adalah pencarian nama untuk tiap track, dan jawabannya tidak berubah
    /// selama klip dan rangkanya tetap.
    struct State {
        const animation::Clip* clip = nullptr;
        const animation::Skeleton* skeleton = nullptr;
        animation::ClipBinding binding;
        animation::Pose pose;
        std::vector<Mat4> palette;
    };

    std::unordered_map<std::string, animation::Skeleton> skeletons_;
    std::unordered_map<std::string, animation::Clip> clips_;
    /// Kunci `uint32_t`, bukan `scene::Entity`: enum class tidak punya hash
    /// bawaan, dan menuliskannya di sini lebih murah daripada spesialisasi
    /// `std::hash` yang berlaku untuk seluruh program.
    std::unordered_map<uint32_t, State> states_;
    int animated_ = 0;
};

}  // namespace sim::editor
