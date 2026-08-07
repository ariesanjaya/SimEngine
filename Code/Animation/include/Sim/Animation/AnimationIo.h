#pragma once

#include "Sim/Animation/Clip.h"
#include "Sim/Core/AssetRef.h"

#include <filesystem>
#include <string>

namespace sim::animation {

inline constexpr int kSkeletonSchemaVersion = 1;
inline constexpr int kClipSchemaVersion = 1;

/// Isi berkas `.simskel`.
///
/// **Seluruh rangka muat di JSON-nya, tidak seperti heightmap atau peta bobot.**
/// Rig 256 bone adalah beberapa puluh kilobyte teks — cukup kecil untuk di-diff
/// dengan berguna, dan justru di situlah letak gunanya: perubahan pada bind pose
/// adalah hal yang perlu dilihat orang di dalam review, bukan gumpalan biner.
struct SkeletonDocument {
    std::string name;
    /// Pemetaan nama bone rangka ini → nama rig standar. Kosong berarti rangka
    /// ini tidak ikut skema retarget mana pun.
    RetargetMap retarget;
};

/// Isi berkas `.simanim`.
struct ClipDocument {
    /// Rangka tempat klip ini ditulis. Disimpan sebagai GUID dengan alasan yang
    /// sama seperti terrain pada vegetasi: memindahkan atau mengganti nama
    /// berkas rangka tidak boleh memutus klipnya.
    AssetRef skeleton;
};

struct AnimationIoResult {
    bool ok = false;
    std::string error;
    int sourceVersion = 0;
};

// --- rangka -------------------------------------------------------------------

/// Keluarannya deterministik — urutan field tetap — supaya menyimpan dokumen
/// yang tidak disunting menghasilkan byte yang sama.
std::string SaveSkeletonToString(const SkeletonDocument& document, const Skeleton& skeleton);
AnimationIoResult LoadSkeletonFromString(SkeletonDocument& document, Skeleton& skeleton,
                                         const std::string& text);
AnimationIoResult SaveSkeleton(const Skeleton& skeleton, const SkeletonDocument& document,
                               const std::filesystem::path& path);
AnimationIoResult LoadSkeleton(Skeleton& skeleton, SkeletonDocument& document,
                               const std::filesystem::path& path);

// --- klip ---------------------------------------------------------------------

std::string SaveClipToString(const ClipDocument& document, const Clip& clip);
AnimationIoResult LoadClipFromString(ClipDocument& document, Clip& clip, const std::string& text);
AnimationIoResult SaveClip(const Clip& clip, const ClipDocument& document,
                           const std::filesystem::path& path);
AnimationIoResult LoadClip(Clip& clip, ClipDocument& document, const std::filesystem::path& path);

}  // namespace sim::animation
