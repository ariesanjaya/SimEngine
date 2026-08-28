#include "Sim/SceneView/SceneView.h"

#include "Sim/Assets/MeshSdfBakery.h"
#include "Sim/Assets/TextureBakery.h"

#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Assets/MaterialImport.h"
#include "Sim/Material/MaterialGraph.h"
#include "Sim/Core/Log.h"
#include "Sim/Material/MaterialInstance.h"
#include "Sim/Material/MaterialNodeCatalog.h"
#include "Sim/Terrain/TerrainDecal.h"
#include "Sim/SceneView/Selection.h"
#include "Sim/Scene/Components.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>

namespace sim::view {
namespace {

constexpr Vec4 kMeshColor{0.62f, 0.65f, 0.70f, 1.0f};

/// Warna terrain sampai materialnya sungguh digambar.
///
/// Sengaja lebih hijau daripada mesh biasa, dan sengaja **bukan** abu-abu:
/// terrain yang sewarna dengan setiap kubus di scene membuat "apakah ini
/// tanahnya atau sebuah blok" jadi pertanyaan yang harus dijawab dengan
/// mengklik.
constexpr Vec4 kTerrainColor{0.45f, 0.52f, 0.38f, 1.0f};
constexpr Vec4 kLightColor{1.00f, 0.86f, 0.42f, 1.0f};
constexpr Vec4 kCameraColor{0.48f, 0.78f, 0.95f, 1.0f};
constexpr Vec4 kEmptyColor{0.45f, 0.47f, 0.52f, 0.85f};

/// Uji sinar terhadap kotak sejajar sumbu dengan metode slab.
///
/// Pembagian dengan komponen arah yang nol menghasilkan inf, dan itu justru
/// perilaku yang benar di sini: sinar yang sejajar sebuah sumbu akan punya
/// t = ±inf pada sumbu itu, sehingga irisan intervalnya tetap benar tanpa
/// pengecekan khusus. Yang perlu dijaga hanya kasus 0/0 → NaN, dan itu
/// tersaring oleh perbandingan di akhir yang selalu false untuk NaN.
bool RayIntersectsAabb(const Vec3& origin, const Vec3& direction, const Vec3& boundsMin,
                       const Vec3& boundsMax, float& outDistance) {
    const Vec3 inverse = 1.0f / direction;
    const Vec3 t0 = (boundsMin - origin) * inverse;
    const Vec3 t1 = (boundsMax - origin) * inverse;
    const Vec3 slabMin = glm::min(t0, t1);
    const Vec3 slabMax = glm::max(t0, t1);

    const float tNear = glm::max(glm::max(slabMin.x, slabMin.y), slabMin.z);
    const float tFar = glm::min(glm::min(slabMax.x, slabMax.y), slabMax.z);

    if (!(tNear <= tFar) || tFar < 0.0f) {
        return false;
    }
    // Sinar yang berawal di dalam kotak tetap mengenainya, jaraknya nol.
    outDistance = tNear >= 0.0f ? tNear : 0.0f;
    return true;
}

/// `"float3(0.62, 0.65, 0.70)"` menjadi sebuah warna.
///
/// **Nilai pin material adalah teks kode shader, bukan angka.** Itu yang
/// membuatnya bisa berupa ekspresi apa pun; yang bisa dibaca di sini hanyalah
/// bentuk harfiahnya, dan yang bukan literal jatuh ke `fallback` alih-alih
/// menjadi tebakan. Satu angka berarti ketiganya sama — `float3(0.5)`.
Vec3 ParseFloat3(std::string_view text, const Vec3& fallback) {
    const std::size_t open = text.find('(');
    const std::size_t close = text.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close <= open) {
        return fallback;
    }
    std::array<float, 3> parsed{};
    int count = 0;
    std::size_t cursor = open + 1;
    while (cursor < close && count < 3) {
        const std::size_t comma = text.find(',', cursor);
        const std::size_t end = comma == std::string_view::npos || comma > close ? close : comma;
        const std::string_view piece = text.substr(cursor, end - cursor);
        float value = 0.0f;
        const char* begin = piece.data();
        const char* stop = piece.data() + piece.size();
        while (begin < stop && (*begin == ' ' || *begin == '\t')) {
            ++begin;
        }
        if (std::from_chars(begin, stop, value).ec != std::errc{}) {
            return fallback;
        }
        parsed[static_cast<std::size_t>(count++)] = value;
        cursor = end + 1;
    }
    if (count == 1) {
        return Vec3(parsed[0]);
    }
    return count == 3 ? Vec3(parsed[0], parsed[1], parsed[2]) : fallback;
}

/// Kubus satuan yang membentang [-0,5, 0,5], sebagai geometri CPU.
///
/// **Cerminan kubus yang digambar renderer untuk `kUnitCubeMesh`**, dan ia harus
/// tetap sama: yang di sana menggambar, yang di sini menghalangi cahaya. Kubus
/// yang berbeda ukuran di kedua sisi menghasilkan bayangan panggang yang tidak
/// sejajar dengan bendanya.
const assets::MeshData& UnitCubeGeometry() {
    static const assets::MeshData cube = [] {
        assets::MeshData data;
        // Enam muka, empat vertex masing-masing: normal per muka menuntut vertex
        // yang tidak dibagi antar-muka.
        const Vec3 faces[6][4] = {
            {{-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}},
            {{0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}},
            {{0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}},
            {{-0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, -0.5f}},
            {{-0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}},
            {{-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, 0.5f}, {-0.5f, -0.5f, 0.5f}},
        };
        const Vec3 normals[6] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
        for (int face = 0; face < 6; ++face) {
            const auto base = static_cast<uint32_t>(data.vertices.size());
            for (int corner = 0; corner < 4; ++corner) {
                assets::MeshVertex vertex;
                vertex.position = faces[face][corner];
                vertex.normal = normals[face];
                data.vertices.push_back(vertex);
            }
            for (const uint32_t offset : {0u, 1u, 2u, 0u, 2u, 3u}) {
                data.indices.push_back(base + offset);
            }
        }
        data.boundsMin = Vec3(-0.5f);
        data.boundsMax = Vec3(0.5f);
        data.parts.push_back(assets::SubMesh{0, static_cast<uint32_t>(data.indices.size()), -1});
        return data;
    }();
    return cube;
}

/// Kunci geometri kubus bawaan. Bukan jalur berkas, jadi ia hanya bisa datang
/// lewat `Adopt` — dan itulah yang dilakukan `Build`.
const char* const kUnitCubeKey = "builtin:unit-cube";

}  // namespace

bool ApplySceneSky(const scene::World& world, render::ViewportDesc& desc) {
    const scene::SkyComponent* sky = nullptr;
    for (const auto raw : world.Registry().view<scene::SkyComponent>()) {
        // **Yang pertama, bukan yang terdekat** — aturan yang sama dengan
        // lampu directional: dua langit di satu adegan adalah kesalahan
        // pengarangan, dan memilih salah satunya menurut jarak hanya membuat
        // kesalahan itu berubah-ubah saat kamera bergerak.
        sky = world.TryGet<scene::SkyComponent>(static_cast<scene::Entity>(raw));
        break;
    }
    desc.skyEnabled = sky != nullptr;
    if (sky == nullptr) {
        return false;
    }
    desc.skySource = sky->source == scene::SkySourceKind::HdrMap ? render::SkySource::HdrMap
                                                                 : render::SkySource::Atmosphere;
    desc.skyIntensity = sky->intensity;
    desc.cameraHeightKm = sky->cameraHeightKm;
    desc.aerialPerspective = sky->aerialPerspective;
    desc.aerialHaze = sky->aerialHaze;
    desc.hdriPath = sky->hdriPath;
    desc.hdriRotation = sky->hdriRotation;
    desc.hdriIntensity = sky->hdriIntensity;
    return true;
}

void ApplyWorldSettings(const scene::World& world, render::ViewportDesc& desc) {
    const scene::WorldSettings& settings = world.Settings();

    // **Cerminan, bukan kebenaran kedua.** `desc.gi.enabled` diturunkan di sini
    // dan tidak disunting di tempat lain — dua tempat menyunting satu hal adalah
    // dua tempat yang suatu saat tidak sepakat, dan panel langit sudah memilih
    // ini dengan benar sekali.
    desc.gi.enabled = settings.indirect == scene::IndirectLighting::RealTime;

    // **`environment` diteruskan sejak B3.** Sebelum itu ia tersimpan di berkas
    // tapi tidak berpengaruh, karena yang memakainya panggangannya dan
    // panggangannya baru bisa membaca berkas di B3.
    desc.environment = settings.environment == scene::EnvironmentSource::File
                           ? render::EnvironmentSource::File
                           : render::EnvironmentSource::Sky;
    desc.extractSunFromEnvironment = settings.extractSun;

    // **Hanya `Precomputed` yang memanggang kisi probe.** `RealTime` menelusuri
    // tiap frame dan tidak punya gunanya; `None` memang tidak menerima cahaya
    // tak-langsung sama sekali. Nol di sini adalah bagaimana keduanya
    // mengatakannya, bukan jarak yang kebetulan kosong.
    desc.probeSpacing = settings.indirect == scene::IndirectLighting::Precomputed
                            ? scene::ProbeSpacingOf(settings)
                            : 0.0f;

    desc.post.exposureMode = settings.exposureMode == scene::ExposureModeKind::Manual
                                 ? render::ExposureMode::Manual
                                 : render::ExposureMode::Automatic;
    desc.post.exposureCompensation = settings.exposureCompensation;
}

std::string ResolveHdriPath(std::string_view hdriPath, std::string_view builtinDir) {
    if (hdriPath.empty() || builtinDir.empty()) {
        return std::string(hdriPath);
    }
    const std::filesystem::path written(hdriPath);
    if (written.is_absolute()) {
        return std::string(hdriPath);
    }
    // Keberadaannya diperiksa di sini, bukan diserahkan ke renderer: yang tidak
    // ada di bawah `Resources` bawaan boleh jadi memang relatif terhadap sesuatu
    // yang lain, dan menempelkan akar di depannya hanya menukar satu jalur yang
    // gagal dengan jalur lain yang gagal — dengan pesan yang tidak lagi menyebut
    // apa yang tertulis di level.
    std::error_code code;
    const std::filesystem::path candidate = std::filesystem::path(builtinDir) / written;
    if (!std::filesystem::exists(candidate, code)) {
        return std::string(hdriPath);
    }
    return candidate.string();
}

void SceneView::Build(scene::World& world, const Selection& selection,
                      const assets::AssetDatabase* assets,
                      render::IViewportRenderer* renderer,
                      const SkinnedPreview* animation,
                      const assets::AssetDatabase* builtinAssets,
                      WhiteboxStore* whiteboxes, const TerrainView& terrainView) {
    // Dipasang di sini supaya `FindAsset` bisa memakainya tanpa menambah
    // parameter kesembilan ke fungsi yang sudah delapan panjangnya.
    builtinAssets_ = builtinAssets;
    ++frame_;
    meshes_.clear();
    skinMatrices_.clear();
    partColors_.clear();
    partTextures_.clear();
    partMaterials_.clear();
    lights_.clear();
    lines_.clear();
    icons_.clear();
    pickables_.clear();
    bakeEntries_.clear();

    // view<> menghasilkan entt::entity, sedangkan scene::Entity adalah enum
    // tersendiri supaya tipe entity kita tidak otomatis tertukar dengan tipe
    // pustaka. Konversinya eksplisit di satu tempat ini saja.
    // Decal yang tidak lagi ada di scene tidak boleh menahan meshnya selamanya.
    // Dibersihkan di awal, bukan di akhir: yang membersihkan sesudah menyusun
    // daftar akan membuang yang baru saja dipakai frame ini.
    for (auto it = decals_.begin(); it != decals_.end();) {
        it = it->second.touched + 2 < frame_ ? decals_.erase(it) : std::next(it);
    }

    for (const auto raw : world.Registry().view<scene::IdComponent>()) {
        const auto entity = static_cast<scene::Entity>(raw);
        const auto* visibility = world.TryGet<scene::VisibilityComponent>(entity);
        if (visibility != nullptr && !visibility->visible) {
            continue;
        }

        const Mat4& matrix = world.WorldMatrix(entity);
        const bool selected = selection.Contains(ToSelectionId(entity));
        // Entity terkunci tetap digambar tapi tidak masuk daftar pickable —
        // itu justru gunanya: latar yang terlihat tanpa terpilih tak sengaja.
        const bool pickable = visibility == nullptr || !visibility->locked;

        if (const auto* decalComponent = world.TryGet<scene::DecalComponent>(entity)) {
            AppendDecal(*decalComponent, entity, matrix, selected, pickable, assets, renderer,
                        terrainView, world);
            continue;
        }

        if (const auto* terrainComponent = world.TryGet<scene::TerrainComponent>(entity)) {
            AppendTerrain(*terrainComponent, entity, matrix, selected, pickable, assets, renderer,
                          terrainView);
            continue;
        }

        const auto* meshRenderer = world.TryGet<scene::MeshRendererComponent>(entity);
        const auto* whiteboxComponent = world.TryGet<scene::WhiteboxComponent>(entity);
        // **Salah satu saja sudah cukup.** Whitebox memberi bentuk dan
        // `MeshRenderer` memberi material; menuntut keduanya berarti blok yang
        // baru dijatuhkan tidak tergambar sama sekali sampai seseorang menebak
        // bahwa ia perlu komponen kedua yang tidak menunjuk mesh apa pun.
        if (meshRenderer != nullptr || whiteboxComponent != nullptr) {
            render::MeshInstance instance;
            instance.transform = matrix;
            // Kubus satuan adalah nilai mundur, bukan satu-satunya pilihan lagi:
            // aset mesh yang bisa dimuat menggantikan batas ini dengan batasnya
            // sendiri. Batas itu dipakai bersama oleh yang digambar dan yang
            // bisa diklik, jadi keduanya tidak mungkin berbeda.
            instance.boundsMin = Vec3(-0.5f);
            instance.boundsMax = Vec3(0.5f);
            instance.color = kMeshColor;
            instance.selected = selected;
            // Kunci geometri CPU-nya, untuk picking presisi (R1). Sama dengan
            // kunci yang dipakai renderer supaya keduanya tidak pernah menunjuk
            // bentuk yang berbeda. **Di luar blok di bawah**, karena yang
            // membacanya adalah daftar pickable di ujung.
            std::string meshKey;
            {
                // Warna mundur untuk ruas yang tidak punya material di mana pun:
                // material bawaan editor.
                instance.color = BuiltinColor(builtinAssets);
                if (meshRenderer != nullptr) {
                    instance.castShadows = meshRenderer->castShadows;
                    instance.receiveShadows = meshRenderer->receiveShadows;
                }

                // GUID → jalur → geometri. **Renderer yang menyimpan cache-nya**,
                // bukan di sini: yang bisa memutuskan sebuah mesh sudah ada di
                // GPU hanyalah yang memegang buffer-nya, dan cache kedua di
                // editor adalah cache yang suatu saat tidak sepakat dengan yang
                // pertama.
                // **Whitebox mendahului aset mesh.** Entity yang membawa keduanya
                // memakai whitebox untuk bentuknya dan `MeshRenderer` untuk
                // materialnya: bentuknya dirancang di sini, materialnya dipilih
                // seperti mesh lain.
                bool fromWhitebox = false;
                if (whiteboxes != nullptr && renderer != nullptr && assets != nullptr &&
                    whiteboxComponent != nullptr && whiteboxComponent->whitebox.IsValid()) {
                    const Uuid guid = whiteboxComponent->whitebox.guid;
                    if (const assets::AssetRecord* record = assets->Find(guid)) {
                        whiteboxes->Get(guid, assets->AbsolutePath(*record));
                    }
                    if (const assets::MeshData* built = whiteboxes->BuiltMesh(guid)) {
                        const render::MeshAsset mesh = renderer->AcquireMeshData(
                            guid.ToString(), *built, whiteboxes->Version(guid));
                        if (mesh.loaded) {
                            instance.mesh = mesh.handle;
                            instance.boundsMin = mesh.boundsMin;
                            instance.boundsMax = mesh.boundsMax;
                            if (meshRenderer != nullptr) {
                                AppendPartColors(*meshRenderer, mesh.partCount, assets, renderer, instance);
                            }
                            fromWhitebox = true;
                            meshKey = guid.ToString();
                            // Whitebox tidak punya berkas untuk diurai, jadi
                            // geometrinya hanya bisa sampai ke jalur sinar lewat
                            // sini. Tanpa baris ini ia tidak menghalangi
                            // panggangan cahaya sama sekali.
                            AdoptGeometry(meshKey, *built, whiteboxes->Version(guid));
                        }
                    }
                }

                if (!fromWhitebox && assets != nullptr && renderer != nullptr &&
                    meshRenderer != nullptr && meshRenderer->mesh.IsValid()) {
                    if (const AssetLookup found = FindAsset(assets, meshRenderer->mesh.guid)) {
                        const std::filesystem::path meshPath =
                            found.database->AbsolutePath(*found.record);
                        const render::MeshAsset mesh =
                            renderer->AcquireMesh(meshPath.string());
                        if (mesh.loaded) {
                            meshKey = meshPath.string();
                        }
                        // **Medan jaraknya diminta di sini, di sebelah
                        // geometrinya.** Keduanya berkunci berkas yang sama, dan
                        // yang meminta salah satunya hampir selalu memerlukan
                        // yang lain. Jawaban pertamanya `Pending` — bake sebuah
                        // gedung memakan detik — dan sampai ia siap, clipmap
                        // memakai kotak batas mesh itu seperti sebelum M1.
                        if (sdfBakery_ != nullptr && mesh.loaded) {
                            const assets::MeshSdfRef field = sdfBakery_->Request(meshPath);
                            if (field.state == assets::MeshSdfState::Ready) {
                                renderer->SetMeshDistanceField(mesh.handle, field.grid);
                            }
                        }
                        // Angkanya dilaporkan balik ke indeks aset. Yang
                        // memuatnya sudah memegangnya, dan menghitungnya lagi
                        // saat impor berarti mengurai berkas yang sama dua kali
                        // — sekali untuk digambar, sekali untuk ditampilkan.
                        //
                        // `assets` const, jadi ini satu-satunya jalur non-const
                        // yang dibenarkan: yang ditulis bukan isi indeks
                        // melainkan catatan tentang isinya.
                        if (mesh.loaded && mesh.triangleCount > 0) {
                            const_cast<assets::AssetDatabase*>(found.database)->ReportMeshStats(
                                meshRenderer->mesh.guid, mesh.triangleCount, mesh.vertexCount);
                        }
                        if (mesh.loaded) {
                            instance.mesh = mesh.handle;
                            instance.boundsMin = mesh.boundsMin;
                            instance.boundsMax = mesh.boundsMax;
                            AppendPartColors(*meshRenderer, mesh.partCount, assets, renderer, instance);
                            AppendSkinPalette(mesh.boneCount,
                                              animation != nullptr
                                                  ? animation->PaletteFor(entity)
                                                  : std::span<const Mat4>{},
                                              instance);
                        }
                    }
                }
            }
            meshes_.push_back(instance);

            // Daftar untuk panggangan cahaya statis (S2): **seluruh geometri
            // bermesh, termasuk yang dikunci.** Mengunci latar statis adalah cara
            // orang menjaganya tidak terpilih tak sengaja, dan latar statis
            // itulah yang menaungi panggangan — alasan yang sama yang membuat
            // `GeometryBounds` memakai `meshes_` alih-alih `pickables_`.
            // **Kubus bawaan ikut menghalangi panggangan.** Ia tidak punya
            // GUID aset dan karena itu tidak punya jalur berkas, jadi sampai
            // sini ia tidak pernah masuk jalur sinar — dan sebuah ruangan yang
            // dibangun darinya tetap disinari langit seolah di luar ruangan.
            //
            // Matriksnya digabung di sini, bukan diserahkan apa adanya: geometri
            // kubusnya membentang [-0,5, 0,5], sedangkan instance-nya diskalakan
            // ke kotak batasnya. Pemetaan yang sama persis dipakai renderer saat
            // menggambar; dua pemetaan yang berselisih menghasilkan bayangan
            // panggang yang tidak sejajar dengan bendanya.
            if (meshKey.empty() && instance.mesh == render::kUnitCubeMesh) {
                AdoptGeometry(kUnitCubeKey, UnitCubeGeometry(), 1);
                const Vec3 centre = (instance.boundsMin + instance.boundsMax) * 0.5f;
                const Vec3 size = glm::max(instance.boundsMax - instance.boundsMin, Vec3(1e-4f));
                const Mat4 composed =
                    glm::scale(glm::translate(matrix, centre), size);
                Vec3 worldMin(std::numeric_limits<float>::max());
                Vec3 worldMax(std::numeric_limits<float>::lowest());
                for (int corner = 0; corner < 8; ++corner) {
                    const Vec3 local((corner & 1) != 0 ? 0.5f : -0.5f,
                                     (corner & 2) != 0 ? 0.5f : -0.5f,
                                     (corner & 4) != 0 ? 0.5f : -0.5f);
                    const Vec3 atCorner = Vec3(composed * Vec4(local, 1.0f));
                    worldMin = glm::min(worldMin, atCorner);
                    worldMax = glm::max(worldMax, atCorner);
                }
                bakeEntries_.push_back(
                    BakeEntry{entity, composed, kUnitCubeKey, worldMin, worldMax});
            }

            if (!meshKey.empty()) {
                // Kedelapan sudutnya ditransformasi: kotak yang diputar tidak
                // lagi sejajar sumbu, dan mentransformasi min/max saja
                // menghasilkan kotak yang lebih kecil daripada isinya — brick
                // yang dibuang karenanya adalah brick yang dipakai.
                Vec3 worldMin(std::numeric_limits<float>::max());
                Vec3 worldMax(std::numeric_limits<float>::lowest());
                for (int corner = 0; corner < 8; ++corner) {
                    const Vec3 local(
                        (corner & 1) != 0 ? instance.boundsMax.x : instance.boundsMin.x,
                        (corner & 2) != 0 ? instance.boundsMax.y : instance.boundsMin.y,
                        (corner & 4) != 0 ? instance.boundsMax.z : instance.boundsMin.z);
                    const Vec3 atCorner = Vec3(matrix * Vec4(local, 1.0f));
                    worldMin = glm::min(worldMin, atCorner);
                    worldMax = glm::max(worldMax, atCorner);
                }
                bakeEntries_.push_back(BakeEntry{entity, matrix, meshKey, worldMin, worldMax});
            }

            if (pickable) {
                pickables_.push_back(Pickable{entity, matrix, instance.boundsMin,
                                              instance.boundsMax, std::move(meshKey)});
            }
            continue;
        }

        EntityIcon icon;
        icon.entity = entity;
        icon.position = Vec3(matrix[3]);
        icon.selected = selected;
        icon.pickable = pickable;

        if (const auto* light = world.TryGet<scene::LightComponent>(entity)) {
            AppendLight(*light, matrix);
            // Lampu directional dibedakan dari yang lain: ia menerangi seluruh
            // scene tanpa punya posisi bermakna, dan membedakannya sekilas
            // menghemat satu klik ke Inspector.
            icon.glyph = light->type == scene::LightType::Directional
                             ? iconGlyphs_.directionalLight
                             : iconGlyphs_.light;
            icon.color = kLightColor;
        } else if (world.Has<scene::CameraComponent>(entity)) {
            icon.glyph = iconGlyphs_.camera;
            icon.color = kCameraColor;
        } else {
            icon.glyph = iconGlyphs_.empty;
            icon.color = kEmptyColor;
        }
        icons_.push_back(icon);
    }
}

/// Menyediakan warna tiap slot material sebuah instance.
///
/// **Slot kosong ditinggalkan ber-alpha nol, bukan diisi warna bawaan di sini.**
/// Nol berarti "tidak ditetapkan", dan renderer lalu memakai material yang
/// tertulis di berkas mesh untuk ruas itu — yang tidak diketahui editor. Mengisi
/// warna bawaan di sini akan menimpanya, dan model yang berkasnya membawa
/// warnanya sendiri akan tergambar abu-abu seluruhnya.
SceneView::AssetLookup SceneView::FindAsset(const assets::AssetDatabase* assets,
                                            const Uuid& guid) const {
    if (!guid.IsValid()) {
        return {};
    }
    if (assets != nullptr) {
        if (const assets::AssetRecord* record = assets->Find(guid)) {
            return {assets, record};
        }
    }
    if (builtinAssets_ != nullptr && builtinAssets_ != assets) {
        if (const assets::AssetRecord* record = builtinAssets_->Find(guid)) {
            return {builtinAssets_, record};
        }
    }
    return {};
}

void SceneView::AppendPartColors(const scene::MeshRendererComponent& renderer, uint32_t partCount,
                                 const assets::AssetDatabase* assets,
                                 render::IViewportRenderer* textureRenderer,
                                 render::MeshInstance& instance) {
    if (partCount == 0) {
        return;
    }
    instance.partFirst = static_cast<uint32_t>(partColors_.size());
    instance.partCount = partCount;
    for (uint32_t slot = 0; slot < partCount; ++slot) {
        // Daftar yang lebih pendek daripada jumlah ruas diperlakukan sebagai
        // slot kosong: menambah material di berkas mesh tidak boleh merusak
        // level yang sudah menyebut sebagian di antaranya.
        const bool assigned = slot < renderer.materials.size() &&
                              renderer.materials[slot].IsValid();
        partColors_.push_back(assigned ? MaterialColor(assets, renderer.materials[slot].guid)
                                       : Vec4(0.0f));
        // Sejajar, selalu. Larik yang panjangnya tidak sama dengan `partColors_`
        // membuat setiap indeks sesudah ruas pertama menunjuk tekstur milik
        // instance lain — dan yang terlihat bukan galat melainkan tekstur yang
        // berpindah objek.
        partTextures_.push_back(assigned ? MaterialTexture(assets, textureRenderer,
                                                           renderer.materials[slot].guid)
                                         : render::kInvalidTexture);
        // Material sungguhannya, kalau bisa dikompilasi. Nol berarti ruas ini
        // digambar jalur mundur `box.frag` — bukan tidak digambar.
        partMaterials_.push_back(assigned ? ForwardMaterial(assets, textureRenderer,
                                                            renderer.materials[slot].guid)
                                          : render::kInvalidMaterial);
    }
}

/// Warna dasar sebuah material, dibaca dari `.simmat`-nya sekali lalu diingat.
///
/// **Yang dibaca hanya warna dasarnya, dan itu memang seluruh yang bisa
/// diperlihatkan sekarang.** Pass forward masih memakai `box.frag`, yang tidak
/// punya roughness, metalness, maupun tekstur; begitu pipeline material
/// menggantikannya, yang di sini berganti menjadi material seutuhnya.
Vec4 SceneView::MaterialColor(const assets::AssetDatabase* assets, const Uuid& guid) {
    if (const auto found = materialColor_.find(guid); found != materialColor_.end()) {
        return found->second;
    }
    Vec4 color = kMeshColor;
    {
        if (const AssetLookup found = FindAsset(assets, guid)) {
            material::MaterialGraph graph;
            if (material::LoadMaterialFromFile(graph,
                                               found.database->AbsolutePath(*found.record)).ok) {
                for (const material::MaterialNode& node : graph.nodes) {
                    if (node.type != material::kSurfaceOutputType) {
                        continue;
                    }
                    const auto pin = node.pinValues.find("baseColor");
                    if (pin != node.pinValues.end()) {
                        color = Vec4(ParseFloat3(pin->second, Vec3(kMeshColor)), 1.0f);
                    }
                    break;
                }
            }
        }
    }
    // Yang gagal dibaca ikut diingat: mengurai berkas rusak tiap frame adalah
    // editor yang melambat tanpa sebab yang terlihat.
    materialColor_.emplace(guid, color);
    return color;
}

/// Tekstur warna dasar sebuah material, dibaca sekali lalu diingat.
///
/// **Menelusuri instance-nya, bukan graph induknya.** Yang menyimpan "gambar
/// mana" adalah `.simmatinst`; induknya hanya menyatakan bahwa slot itu ada.
/// Membaca induk saja akan menghasilkan tekstur yang sama untuk seluruh material
/// impor — yaitu tidak ada.
render::TextureHandle SceneView::MaterialTexture(const assets::AssetDatabase* assets,
                                                 render::IViewportRenderer* renderer,
                                                 const Uuid& guid) {
    if (assets == nullptr || renderer == nullptr) {
        return render::kInvalidTexture;
    }

    Uuid texture;
    if (const auto found = materialTexture_.find(guid); found != materialTexture_.end()) {
        texture = found->second;
    } else {
        const AssetLookup owner = FindAsset(assets, guid);
        material::MaterialInstance instance;
        if (owner && material::LoadInstanceFromFile(
                         instance, owner.database->AbsolutePath(*owner.record))
                         .ok) {
            // Nama parameternya disebut satu tempat, dipakai importir maupun di
            // sini: dua ejaan berarti tekstur yang tersimpan tetapi tidak pernah
            // terpasang, dan tidak ada satu pun galat yang menyertainya.
            texture = instance.Texture(assets::kBaseColorTextureParameter);
        }
        // Yang gagal dibaca ikut diingat, alasan yang sama dengan warnanya:
        // mengurai berkas rusak tiap frame adalah editor yang melambat tanpa
        // sebab yang terlihat. Sebuah `.simmat` yang dibuka di sini juga wajar
        // gagal — ia induk, bukan instance — dan itu bukan galat.
        materialTexture_.emplace(guid, texture);
    }

    if (!texture.IsValid()) {
        return render::kInvalidTexture;
    }
    return UploadedTexture(assets, renderer, texture);
}

/// Sebuah aset gambar menjadi handle tekstur — lewat baker, tidak pernah
/// langsung.
///
/// **Jalur ini yang menjaga renderer tidak pernah mendekode gambar.** Yang
/// diserahkan ke `AcquireTexture` adalah `.ktx2` di dalam cache; berkas
/// sumbernya berhenti di sini.
render::TextureHandle SceneView::UploadedTexture(const assets::AssetDatabase* assets,
                                                 render::IViewportRenderer* renderer,
                                                 const Uuid& image) {
    if (assets == nullptr || renderer == nullptr || bakery_ == nullptr) {
        return render::kInvalidTexture;
    }
    const AssetLookup found = FindAsset(assets, image);
    if (!found) {
        return render::kInvalidTexture;
    }
    // **Yang sudah `.ktx2` diteruskan apa adanya.** Itulah bentuk tekstur di
    // project hasil cook: sumbernya sengaja tidak ikut dikirim, jadi memanggang
    // ulang bukan sekadar sia-sia — tidak ada yang bisa dipanggang. Bakery tetap
    // jalur untuk project yang sedang disunting, di mana sumbernya memang ada.
    const std::filesystem::path resolved = found.database->AbsolutePath(*found.record);
    if (resolved.extension() == ".ktx2") {
        return renderer->AcquireTexture(resolved.string());
    }
    const assets::BakedTextureRef baked = bakery_->Request(resolved);
    switch (baked.state) {
        case assets::BakeState::Ready:
            return renderer->AcquireTexture(baked.path.string());
        case assets::BakeState::Pending:
            // Placeholder, bukan putih. Ruas ini **punya** tekstur; menggambarnya
            // putih membuatnya tidak bisa dibedakan dari ruas yang memang tidak
            // bertekstur, dan yang menunggu tanpa tahu ia menunggu akan mengira
            // teksturnya hilang.
            return renderer->PendingTexture();
        case assets::BakeState::Failed:
            break;
    }
    return render::kInvalidTexture;
}

/// Material sebuah ruas, lewat `MaterialPrograms`.
///
/// **Yang belum selesai dikompilasi menjawab nol, dan nol berarti jalur
/// mundur.** Ruas itu tetap tergambar — `box.frag` dengan albedonya — jadi yang
/// terlihat saat sebuah level dibuka adalah permukaan yang berubah rupa begitu
/// materialnya siap, bukan permukaan yang muncul dari ketiadaan.
render::MaterialHandle SceneView::ForwardMaterial(const assets::AssetDatabase* assets,
                                                  render::IViewportRenderer* renderer,
                                                  const Uuid& guid) {
    if (assets == nullptr || renderer == nullptr || materialPrograms_ == nullptr) {
        return render::kInvalidMaterial;
    }
    // Indeks yang memiliki materialnya, bukan selalu indeks project: material
    // bawaan menyebut induk yang juga bawaan, dan yang mencarinya harus indeks
    // yang sama.
    const AssetLookup owner = FindAsset(assets, guid);
    if (!owner) {
        return render::kInvalidMaterial;
    }
    const MaterialProgramRef program = materialPrograms_->Request(
        *owner.database, guid, *renderer, [&](const Uuid& image) -> ResolvedMaterialTexture {
            ResolvedMaterialTexture resolved;
            resolved.handle = UploadedTexture(assets, renderer, image);
            // Siap berarti bukan placeholder: `UploadedTexture` menjawab
            // placeholder magenta selama bake-nya berjalan, dan handle itu tidak
            // boleh terkunci ke dalam descriptor set material.
            resolved.ready = renderer->PendingTexture() == render::kInvalidTexture ||
                             resolved.handle != renderer->PendingTexture();
            return resolved;
        });
    return program.state == MaterialProgramState::Ready ? program.handle
                                                        : render::kInvalidMaterial;
}

/// Warna material bawaan editor — yang mengisi mesh tanpa material sendiri.
Vec4 SceneView::BuiltinColor(const assets::AssetDatabase* builtinAssets) {
    if (builtinAssets == nullptr) {
        return kMeshColor;
    }
    const assets::AssetRecord* record =
        builtinAssets->FindByRelativePath("Materials/Default.simmat");
    return record != nullptr ? MaterialColor(builtinAssets, record->guid) : kMeshColor;
}

/// Menyediakan palet kulit sebuah instance dan menunjuknya dari instance itu.
///
/// **Palet yang panjangnya tidak cocok ditolak, bukan dipotong atau dipanjangkan.**
/// Panjang yang berbeda berarti pose itu milik rangka yang lain — dan rangka
/// yang lain berarti indeks bone yang menunjuk tulang yang salah. Bind pose yang
/// jelas-jelas diam jauh lebih mudah dilacak daripada kulit yang terpelintir.
///
/// Nilai mundurnya matriks satuan, yaitu bind pose: matriks kulit adalah
/// `global × invers bind`, dan pada bind pose keduanya saling meniadakan.
///
/// **Diisi walaupun hasilnya sama dengan tidak diisi.** Jalur berkulit yang
/// tidak pernah dijalankan siapa pun adalah jalur yang cacatnya baru ditemukan
/// pada hari klip pertama masuk; diisi begini, seluruh rantainya — buffer skin,
/// palet, pipeline, pass bayangan — berjalan di setiap frame editor, dan
/// kesalahan apa pun di dalamnya langsung terlihat sebagai karakter yang cacat.
void SceneView::AppendSkinPalette(uint32_t boneCount, std::span<const Mat4> palette,
                                  render::MeshInstance& instance) {
    if (boneCount == 0) {
        return;
    }
    instance.skinFirst = static_cast<uint32_t>(skinMatrices_.size());
    instance.skinCount = boneCount;
    if (palette.size() == boneCount) {
        skinMatrices_.insert(skinMatrices_.end(), palette.begin(), palette.end());
        return;
    }
    skinMatrices_.insert(skinMatrices_.end(), boneCount, Mat4(1.0f));
}

/// Menerjemahkan sebuah `LightComponent` menjadi lampu ruang dunia.
///
/// **Di sini, bukan di renderer.** Renderer tidak boleh mengenal tipe komponen
/// — itu seam #1 di docs/ARCHITECTURE.md — dan yang paling mudah bocor lewat
/// batas itu justru penerjemahan seperti ini: bagaimana rotasi entity menjadi
/// arah pancar, dan bagaimana sudut kerucut menjadi kosinus.
terrain::DecalProjection ProjectDecal(const scene::DecalComponent& decal, const Mat4& local) {
    terrain::DecalProjection projection;
    projection.center = Vec3(local[3]);
    // Skala X dan Z menentukan jejaknya — panjang kolom matriksnya, bukan medan
    // skala yang sudah hilang begitu transformnya menjadi matriks.
    projection.halfSize =
        Vec2(glm::length(Vec3(local[0])) * 0.5f, glm::length(Vec3(local[2])) * 0.5f);
    // Rotasi terhadap Y saja, dibaca dari arah sumbu X-nya yang diproyeksikan ke
    // bidang datar. Kemiringan diabaikan, bukan dipaksakan: decal terrain yang
    // menyamping tidak punya arti.
    const Vec3 axis = Vec3(local[0]);
    projection.rotationY = std::atan2(axis.z, axis.x);
    projection.lift = decal.lift;
    projection.color = decal.color;
    projection.maxSteps = decal.maxSteps;
    return projection;
}

/// Terrain pertama yang ditemukan di dunia, atau entity tak sah.
///
/// **Yang pertama, bukan yang terdekat.** Aturan yang sama dengan `FindSky` di
/// ViewportPanel, dan karena alasan yang sama: dua terrain yang tumpang tindih
/// di satu level adalah kesalahan penyusunan, dan memilih "yang terdekat" akan
/// membuat decal berpindah tuan rumah ketika seseorang menggesernya sedikit.
static scene::Entity FindTerrainHost(const scene::World& world, scene::Entity decal) {
    // Induknya lebih dulu: itu cara menyatakan tuan rumah secara eksplisit, dan
    // yang eksplisit harus menang atas yang ditebak.
    const scene::Entity parent = world.ParentOf(decal);
    if (world.IsAlive(parent) && world.TryGet<scene::TerrainComponent>(parent) != nullptr) {
        return parent;
    }
    for (const auto raw : world.Registry().view<scene::TerrainComponent>()) {
        return static_cast<scene::Entity>(raw);
    }
    return scene::kNullEntity;
}

void SceneView::AppendDecal(const scene::DecalComponent& component, scene::Entity entity,
                            const Mat4& matrix, bool selected, bool pickable,
                            const assets::AssetDatabase* assets,
                            render::IViewportRenderer* renderer, const TerrainView& view,
                            scene::World& world) {
    if (view.store == nullptr || assets == nullptr || renderer == nullptr) {
        return;
    }
    const scene::Entity host = FindTerrainHost(world, entity);
    if (!world.IsAlive(host)) {
        return;  // tidak ada terrain untuk ditempeli
    }
    const auto* terrainComponent = world.TryGet<scene::TerrainComponent>(host);
    if (terrainComponent == nullptr || !terrainComponent->terrain.IsValid()) {
        return;
    }
    const assets::AssetRecord* record = assets->Find(terrainComponent->terrain.guid);
    if (record == nullptr) {
        return;
    }
    terrain::Terrain* map =
        view.store->Get(terrainComponent->terrain.guid, assets->AbsolutePath(*record));
    if (map == nullptr) {
        return;
    }

    // Decal berada di ruang dunia; terrain punya transformnya sendiri. Jejaknya
    // dihitung di ruang terrain, karena di situlah heightmap-nya tinggal.
    const Mat4 hostWorld = world.WorldMatrix(host);
    const Mat4 toTerrain = glm::inverse(hostWorld);
    const Mat4 local = toTerrain * matrix;

    const terrain::DecalProjection projection = ProjectDecal(component, local);

    CachedDecal& cached = decals_[ToSelectionId(entity)];
    cached.touched = frame_;
    // Revisi ubin yang memuat pusatnya: memahat di bawah decal harus
    // membangunnya ulang, mengecat tidak.
    const terrain::TerrainDesc& desc = map->Desc();
    const float tileSize = static_cast<float>(desc.tileSamples) * desc.sampleSpacing;
    const uint32_t terrainRevision = map->TileRevision(
        static_cast<int>(projection.center.x / std::max(tileSize, 1e-3f)),
        static_cast<int>(projection.center.z / std::max(tileSize, 1e-3f)));

    if (cached.builtTransform != local || cached.builtColor != projection.color ||
        cached.builtLift != projection.lift || cached.builtSteps != projection.maxSteps ||
        cached.builtTerrain != terrainRevision) {
        cached.mesh = terrain::BuildDecalMesh(*map, projection);
        cached.builtTransform = local;
        cached.builtColor = projection.color;
        cached.builtLift = projection.lift;
        cached.builtSteps = projection.maxSteps;
        cached.builtTerrain = terrainRevision;
        ++cached.upload;
    }
    if (!cached.mesh.IsValid()) {
        return;
    }

    const std::string key = "decal:" + std::to_string(ToSelectionId(entity));
    const render::MeshAsset mesh = renderer->AcquireMeshData(key, cached.mesh, cached.upload);
    if (!mesh.loaded) {
        return;
    }

    // Tekstur decal, bila ada. Ruas tunggal, jadi satu entri warna dan satu
    // entri tekstur.
    render::TextureHandle texture = render::kInvalidTexture;
    if (component.texture.IsValid()) {
        texture = UploadedTexture(assets, renderer, component.texture.guid);
    }

    render::MeshInstance instance;
    // Transform tuan rumahnya, bukan transform decal: geometrinya sudah berada
    // di ruang terrain, dan memakai transform decal akan menerapkan skalanya
    // dua kali.
    instance.transform = hostWorld;
    instance.partFirst = static_cast<uint32_t>(partColors_.size());
    instance.partCount = 1;
    // Warna putih di slot ruasnya: yang mewarnai decal adalah warna simpulnya,
    // yang sudah dipanggang `BuildDecalMesh`. Menaruh warnanya di sini juga
    // berarti mengalikannya dua kali.
    partColors_.push_back(Vec4(1.0f));
    partTextures_.push_back(texture);
    partMaterials_.push_back(render::kInvalidMaterial);
    instance.mesh = mesh.handle;
    instance.boundsMin = mesh.boundsMin;
    instance.boundsMax = mesh.boundsMax;
    instance.color = Vec4(1.0f);
    instance.selected = selected;
    // Decal tidak menjatuhkan bayangan: ia selembar kulit di atas tanah, dan
    // bayangan yang dijatuhkannya akan mendarat di permukaan yang sama persis —
    // menghitamkan dirinya sendiri.
    instance.castShadows = false;
    meshes_.push_back(instance);

    if (pickable) {
        // Kunci geometri sengaja kosong: decal selembar kulit di atas permukaan
        // lain, dan memilihnya per segitiga berarti mengklik lantai di bawahnya
        // memilih decal-nya.
        pickables_.push_back(Pickable{entity, hostWorld, mesh.boundsMin, mesh.boundsMax, {}});
    }
}

void SceneView::AppendTerrain(const scene::TerrainComponent& component, scene::Entity entity,
                              const Mat4& matrix, bool selected, bool pickable,
                              const assets::AssetDatabase* assets,
                              render::IViewportRenderer* renderer,
                              const TerrainView& view) {
    if (view.store == nullptr || assets == nullptr || renderer == nullptr ||
        !component.terrain.IsValid()) {
        return;
    }
    const assets::AssetRecord* record = assets->Find(component.terrain.guid);
    if (record == nullptr) {
        return;
    }
    const terrain::Terrain* loaded =
        view.store->Get(component.terrain.guid, assets->AbsolutePath(*record));
    if (loaded == nullptr) {
        return;
    }

    const terrain::TerrainDesc& desc = loaded->Desc();
    const float tileSize = static_cast<float>(desc.tileSamples) * desc.sampleSpacing;

    // Posisi kamera dipindahkan ke ruang terrain sekali, bukan tiap ubin
    // dipindahkan ke ruang dunia. Satu matriks kali satu titik, bukan sekali
    // per ubin — dan jaraknya sama karena keduanya diukur di ruang yang sama.
    const Mat4 inverse = glm::inverse(matrix);
    const Vec3 eye = Vec3(inverse * Vec4(view.cameraPosition, 1.0f));

    // Dua lintasan: LOD seluruh ubin dulu, baru meshnya. Menjahit tepi menuntut
    // LOD tetangga, dan yang menghitungnya sambil jalan hanya tahu LOD ubin
    // yang sudah lewat — separuh jahitannya akan memakai angka yang belum ada.
    std::vector<int> lods(static_cast<std::size_t>(desc.tilesX * desc.tilesY), 0);
    for (int ty = 0; ty < desc.tilesY; ++ty) {
        for (int tx = 0; tx < desc.tilesX; ++tx) {
            const Vec3 center((static_cast<float>(tx) + 0.5f) * tileSize,
                              loaded->Desc().baseHeight,
                              (static_cast<float>(ty) + 0.5f) * tileSize);
            const float distance = glm::length(Vec3(center.x - eye.x, 0.0f, center.z - eye.z));
            lods[static_cast<std::size_t>(ty * desc.tilesX + tx)] =
                terrain::SelectLod(distance, tileSize, view.maxLod, view.quality);
        }
    }

    const auto lodAt = [&](int tx, int ty) {
        if (tx < 0 || ty < 0 || tx >= desc.tilesX || ty >= desc.tilesY) {
            // Di luar peta tidak ada yang perlu dijahit: tepi peta memang tepi.
            return -1;
        }
        return lods[static_cast<std::size_t>(ty * desc.tilesX + tx)];
    };

    for (int ty = 0; ty < desc.tilesY; ++ty) {
        for (int tx = 0; tx < desc.tilesX; ++tx) {
            const int lod = lodAt(tx, ty);
            terrain::TileNeighborLods neighbors;
            neighbors.negativeX = lodAt(tx - 1, ty);
            neighbors.positiveX = lodAt(tx + 1, ty);
            neighbors.negativeY = lodAt(tx, ty - 1);
            neighbors.positiveY = lodAt(tx, ty + 1);

            const assets::MeshData* data =
                view.store->TileMesh(component.terrain.guid, tx, ty, lod, neighbors);
            if (data == nullptr || !data->IsValid()) {
                continue;  // ubin yang seluruhnya berlubang
            }

            // Kuncinya menyebut ubinnya, bukan terrainnya: satu kunci untuk
            // seluruh peta berarti keenam puluh empat ubin saling menimpa di
            // cache renderer, dan yang tergambar adalah ubin mana pun yang
            // kebetulan terakhir diunggah.
            const std::string key = component.terrain.guid.ToString() + "#" +
                                    std::to_string(tx) + "," + std::to_string(ty);
            // Ubin terrain juga lahir di dalam editor: tidak ada berkas untuk
            // diurai, jadi geometrinya hanya bisa sampai ke jalur sinar lewat
            // sini. Tanpa ini terrain tidak menghalangi panggangan cahaya sama
            // sekali — sebuah lembah tidak menaungi apa pun.
            AdoptGeometry(key, *data, view.store->TileUpload(component.terrain.guid, tx, ty));

            const render::MeshAsset mesh = renderer->AcquireMeshData(
                key, *data, view.store->TileUpload(component.terrain.guid, tx, ty));
            if (!mesh.loaded) {
                continue;
            }

            render::MeshInstance instance;
            instance.transform = matrix;
            instance.mesh = mesh.handle;
            instance.boundsMin = mesh.boundsMin;
            instance.boundsMax = mesh.boundsMax;
            instance.color = kTerrainColor;
            instance.selected = selected;
            meshes_.push_back(instance);

            // **Masuk daftar panggangan, walaupun tidak masuk daftar pickable.**
            // Keduanya menjawab pertanyaan yang berbeda: picking terrain punya
            // jalur heightmap-nya sendiri yang eksak, sedangkan panggangan
            // cahaya menuntut segitiganya benar-benar ada di jalur sinar.
            {
                Vec3 worldMin(std::numeric_limits<float>::max());
                Vec3 worldMax(std::numeric_limits<float>::lowest());
                for (int corner = 0; corner < 8; ++corner) {
                    const Vec3 local((corner & 1) != 0 ? mesh.boundsMax.x : mesh.boundsMin.x,
                                     (corner & 2) != 0 ? mesh.boundsMax.y : mesh.boundsMin.y,
                                     (corner & 4) != 0 ? mesh.boundsMax.z : mesh.boundsMin.z);
                    const Vec3 atCorner = Vec3(matrix * Vec4(local, 1.0f));
                    worldMin = glm::min(worldMin, atCorner);
                    worldMax = glm::max(worldMax, atCorner);
                }
                bakeEntries_.push_back(BakeEntry{entity, matrix, key, worldMin, worldMax});
            }

            if (pickable) {
                // Terrain punya jalur pickingnya sendiri lewat heightmap, yang
                // eksak sekaligus lebih murah daripada ray cast mana pun —
                // lihat batasnya di docs/PLAN-EMBREE.md.
                pickables_.push_back(
                    Pickable{entity, matrix, mesh.boundsMin, mesh.boundsMax, {}});
            }
        }
    }
}

void SceneView::AppendLight(const scene::LightComponent& light, const Mat4& matrix) {
    render::LightInstance instance;
    instance.position = Vec3(matrix[3]);
    // -Z lokal, sama dengan arah hadap kamera. Sumbu yang berbeda antara lampu
    // dan kamera berarti gizmo yang sama menunjuk ke arah yang berbeda
    // bergantung apa yang terpilih.
    const Vec3 forward = glm::normalize(Vec3(matrix * Vec4(0.0f, 0.0f, -1.0f, 0.0f)));
    instance.color = light.color;
    instance.intensity = light.intensity;
    instance.range = light.range;
    instance.sourceRadius = light.sourceRadius;
    instance.castShadows = light.castShadows;

    switch (light.type) {
        case scene::LightType::Directional:
            instance.kind = render::LightKind::Directional;
            // Dibalik: yang disimpan arah **ke** cahaya, sedangkan yang dihadapi
            // entity adalah arah pancarnya.
            instance.direction = -forward;
            break;
        case scene::LightType::Point:
            instance.kind = render::LightKind::Point;
            break;
        case scene::LightType::Spot:
            instance.kind = render::LightKind::Spot;
            instance.direction = forward;
            break;
    }
    // Kerucut dalam harus selalu di dalam kerucut luar. Sudut yang tertukar
    // menghasilkan pembagi negatif di shader, dan tepi berkasnya menyala alih-alih
    // memudar.
    //
    // Keduanya diambil dari pasangan yang sama, bukan yang satu dari yang lain.
    // Bentuk pertama saya menjepit `inner` terhadap `outer` yang sudah menjadi
    // maksimum — hasilnya kedua sudut menjadi sama persis, pembaginya nol, dan
    // kerucutnya kehilangan seluruh gradasinya. Ditemukan test, bukan dengan
    // membaca ulang.
    const float outer = std::max(light.outerAngleRadians, light.innerAngleRadians);
    const float inner = std::min(light.outerAngleRadians, light.innerAngleRadians);
    instance.cosOuter = std::cos(outer);
    instance.cosInner = std::cos(inner);
    lights_.push_back(instance);
}

void SceneView::AddWireBox(const Vec3& boxMin, const Vec3& boxMax, const Vec4& color) {
    // Dua belas rusuk, disusun dari delapan pojok. Pojoknya diindeks lewat bit
    // supaya ketiga sumbunya diperlakukan sama — daftar rusuk yang ditulis
    // tangan adalah tempat satu rusuk hilang tanpa ada yang menyadarinya.
    const auto corner = [&](int index) {
        return Vec3((index & 1) != 0 ? boxMax.x : boxMin.x,
                    (index & 2) != 0 ? boxMax.y : boxMin.y,
                    (index & 4) != 0 ? boxMax.z : boxMin.z);
    };
    for (int index = 0; index < 8; ++index) {
        for (int axis = 0; axis < 3; ++axis) {
            const int bit = 1 << axis;
            // Hanya arah naik, supaya tiap rusuk digambar sekali dan bukan dua
            // kali — rusuk ganda menggandakan biaya dan menebalkan garisnya di
            // tempat yang tidak beraturan.
            if ((index & bit) != 0) {
                continue;
            }
            lines_.push_back(
                render::LineSegment{corner(index), corner(index | bit), color,
                                    linesThroughGeometry_});
        }
    }
}

void SceneView::AddLine(const Vec3& from, const Vec3& to, const Vec4& color) {
    lines_.push_back(render::LineSegment{from, to, color, linesThroughGeometry_});
}

namespace {

/// Titik lingkaran ke-`step` dari `steps`, di bidang yang direntang `axisA` dan
/// `axisB`.
Vec3 CirclePoint(const Vec3& center, const Vec3& axisA, const Vec3& axisB, float radius,
                 int step, int steps) {
    const float angle = 2.0f * kPi * static_cast<float>(step) / static_cast<float>(steps);
    return center + axisA * (radius * std::cos(angle)) + axisB * (radius * std::sin(angle));
}

/// Sudut lingkaran, 32 ruas.
///
/// Cukup halus untuk terbaca sebagai lingkaran pada ukuran layar mana pun yang
/// masuk akal, dan kebetulan sama dengan jumlah sisi yang dipakai
/// `CookCylinder` — jadi rangka kawat silinder punya sebanyak sisi yang
/// benar-benar dimasak PhysX, bukan lebih halus daripada bentuk aslinya.
constexpr int kCircleSteps = 32;

}  // namespace

void SceneView::AddWireBox(const Mat4& transform, const Vec3& halfExtents, const Vec4& color) {
    const auto corner = [&](int index) {
        const Vec3 local((index & 1) != 0 ? halfExtents.x : -halfExtents.x,
                         (index & 2) != 0 ? halfExtents.y : -halfExtents.y,
                         (index & 4) != 0 ? halfExtents.z : -halfExtents.z);
        return Vec3(transform * Vec4(local, 1.0f));
    };
    for (int index = 0; index < 8; ++index) {
        for (int axis = 0; axis < 3; ++axis) {
            const int bit = 1 << axis;
            if ((index & bit) != 0) {
                continue;
            }
            AddLine(corner(index), corner(index | bit), color);
        }
    }
}

void SceneView::AddWireSphere(const Mat4& transform, float radius, const Vec4& color) {
    const Vec3 center(transform[3]);
    const Vec3 axes[3] = {Vec3(transform[0]), Vec3(transform[1]), Vec3(transform[2])};
    // Tiga lingkaran besar, satu per bidang sumbu. Bola yang digambar satu
    // lingkaran terlihat sama dari segala arah dan karena itu tidak
    // memperlihatkan putaran apa pun; tiga sudah cukup untuk membacanya sebagai
    // bola tanpa menjadi bola benang.
    for (int plane = 0; plane < 3; ++plane) {
        const Vec3& a = axes[plane];
        const Vec3& b = axes[(plane + 1) % 3];
        for (int step = 0; step < kCircleSteps; ++step) {
            AddLine(CirclePoint(center, a, b, radius, step, kCircleSteps),
                    CirclePoint(center, a, b, radius, step + 1, kCircleSteps), color);
        }
    }
}

void SceneView::AddWireCapsule(const Mat4& transform, float radius, float halfHeight,
                               const Vec4& color) {
    const Vec3 center(transform[3]);
    const Vec3 axis(transform[0]);   // sumbu X lokal: konvensi PhysX
    const Vec3 up(transform[1]);
    const Vec3 side(transform[2]);
    const Vec3 capA = center - axis * halfHeight;
    const Vec3 capB = center + axis * halfHeight;

    // Dua lingkaran tudung, tegak lurus sumbunya.
    for (int step = 0; step < kCircleSteps; ++step) {
        AddLine(CirclePoint(capA, up, side, radius, step, kCircleSteps),
                CirclePoint(capA, up, side, radius, step + 1, kCircleSteps), color);
        AddLine(CirclePoint(capB, up, side, radius, step, kCircleSteps),
                CirclePoint(capB, up, side, radius, step + 1, kCircleSteps), color);
    }
    // Empat garis sisi, di kedua bidang yang memuat sumbunya.
    for (const Vec3& radial : {up, -up, side, -side}) {
        AddLine(capA + radial * radius, capB + radial * radius, color);
    }
    // Tudung setengah bola. **Inilah yang membedakan kapsul dari silinder di
    // layar**, dan tanpanya sebuah kapsul setinggi 2 m terbaca sebagai silinder
    // setinggi 1,4 m — persis kesalahan yang membuat orang membuka gizmo.
    constexpr int kCapSteps = kCircleSteps / 4;
    for (int half = 0; half < 2; ++half) {
        const Vec3 tip = half == 0 ? capA : capB;
        const Vec3 outward = half == 0 ? -axis : axis;
        for (const Vec3& radial : {up, side}) {
            for (int step = 0; step < kCapSteps; ++step) {
                const float angleA = 0.5f * kPi * static_cast<float>(step) /
                                     static_cast<float>(kCapSteps);
                const float angleB = 0.5f * kPi * static_cast<float>(step + 1) /
                                     static_cast<float>(kCapSteps);
                const auto at = [&](float angle, float sign) {
                    return tip + outward * (radius * std::sin(angle)) +
                           radial * (sign * radius * std::cos(angle));
                };
                AddLine(at(angleA, 1.0f), at(angleB, 1.0f), color);
                AddLine(at(angleA, -1.0f), at(angleB, -1.0f), color);
            }
        }
    }
}

void SceneView::AddWireCylinder(const Mat4& transform, float radius, float halfHeight,
                                const Vec4& color) {
    const Vec3 center(transform[3]);
    const Vec3 axis(transform[0]);   // sumbu X lokal, sama dengan `CookCylinder`
    const Vec3 up(transform[1]);
    const Vec3 side(transform[2]);
    const Vec3 capA = center - axis * halfHeight;
    const Vec3 capB = center + axis * halfHeight;
    for (int step = 0; step < kCircleSteps; ++step) {
        const Vec3 a0 = CirclePoint(capA, up, side, radius, step, kCircleSteps);
        const Vec3 a1 = CirclePoint(capA, up, side, radius, step + 1, kCircleSteps);
        const Vec3 b0 = CirclePoint(capB, up, side, radius, step, kCircleSteps);
        const Vec3 b1 = CirclePoint(capB, up, side, radius, step + 1, kCircleSteps);
        AddLine(a0, a1, color);
        AddLine(b0, b1, color);
        // Rusuk sisi tiap seperempat lingkaran saja: tiga puluh dua rusuk
        // membuat silinder tampak sebagai tabung padat dan menutupi apa yang ada
        // di dalamnya.
        if (step % (kCircleSteps / 4) == 0) {
            AddLine(a0, b0, color);
        }
    }
}

void SceneView::AddWirePlane(const Mat4& transform, float extent, const Vec4& color) {
    const Vec3 center(transform[3]);
    const Vec3 normal(transform[0]);  // +X lokal: konvensi PxPlaneGeometry
    const Vec3 up(transform[1]);
    const Vec3 side(transform[2]);

    // Kisi, bukan kotak kosong. Bidangnya tak hingga; sebuah kotak berpinggir
    // terbaca sebagai lantai seukuran kotak itu, sementara kisi yang terpotong
    // di tepinya terbaca sebagai "berlanjut".
    constexpr int kGrid = 4;
    for (int index = -kGrid; index <= kGrid; ++index) {
        const float offset = extent * static_cast<float>(index) / static_cast<float>(kGrid);
        AddLine(center + up * offset - side * extent, center + up * offset + side * extent,
                color);
        AddLine(center + side * offset - up * extent, center + side * offset + up * extent,
                color);
    }
    // Normal: satu-satunya yang memberi tahu sisi mana yang padat.
    AddLine(center, center + normal * (extent * 0.5f), color);
}

void SceneView::AddWireCircle(const Vec3& center, const Vec3& axisA, const Vec3& axisB,
                              float radius, const Vec4& color) {
    for (int step = 0; step < kCircleSteps; ++step) {
        AddLine(CirclePoint(center, axisA, axisB, radius, step, kCircleSteps),
                CirclePoint(center, axisA, axisB, radius, step + 1, kCircleSteps), color);
    }
}

void SceneView::AddWireCone(const Vec3& apex, const Vec3& direction, float halfAngle,
                            float length, const Vec4& color) {
    const float axisLength = glm::length(direction);
    if (axisLength < 1e-6f || length <= 0.0f) {
        return;
    }
    const Vec3 axis = direction / axisLength;
    // Bingkai tegak lurus sumbu. Sumbu bantu dipilih yang paling tidak sejajar,
    // karena hasil silang dua vektor yang hampir sejajar panjangnya mendekati
    // nol dan arahnya ditentukan pembulatan.
    const Vec3 helper = std::abs(axis.y) < 0.99f ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(1.0f, 0.0f, 0.0f);
    const Vec3 side = glm::normalize(glm::cross(helper, axis));
    const Vec3 up = glm::cross(axis, side);

    const Vec3 rim = apex + axis * length;
    const float radius = length * std::tan(std::clamp(halfAngle, 0.0f, 1.5f));
    AddWireCircle(rim, side, up, radius, color);
    // Empat rusuk saja. Setiap rusuk lingkaran menghasilkan kerucut padat yang
    // menutupi apa yang ada di dalamnya — dan yang ada di dalamnya justru yang
    // sedang disinari.
    for (const Vec3& radial : {side, -side, up, -up}) {
        AddLine(apex, rim + radial * radius, color);
    }
}

void SceneView::AddWireCross(const Vec3& center, float size, const Vec4& color) {
    AddLine(center - Vec3(size, 0.0f, 0.0f), center + Vec3(size, 0.0f, 0.0f), color);
    AddLine(center - Vec3(0.0f, size, 0.0f), center + Vec3(0.0f, size, 0.0f), color);
    AddLine(center - Vec3(0.0f, 0.0f, size), center + Vec3(0.0f, 0.0f, size), color);
}

render::ViewportScene SceneView::Scene() const {
    render::ViewportScene scene;
    scene.meshes = meshes_;
    scene.skinMatrices = skinMatrices_;
    scene.partColors = partColors_;
    scene.partTextures = partTextures_;
    scene.partMaterials = partMaterials_;
    scene.lines = lines_;
    scene.lights = lights_;
    return scene;
}

scene::Entity SceneView::PickIcon(const Mat4& viewProjection, const Vec2& origin, const Vec2& size,
                                  const Vec2& point, float radiusPixels) const {
    scene::Entity best = scene::kNullEntity;
    float bestDistance = radiusPixels * radiusPixels;

    for (const EntityIcon& icon : icons_) {
        if (!icon.pickable) {
            continue;
        }
        Vec2 screen;
        if (!WorldToScreen(viewProjection, origin, size, icon.position, screen)) {
            continue;
        }
        const Vec2 delta = screen - point;
        const float distance = glm::dot(delta, delta);
        if (distance <= bestDistance) {
            bestDistance = distance;
            best = icon.entity;
        }
    }
    return best;
}

scene::Entity SceneView::Raycast(const Ray& ray) const {
    scene::Entity best = scene::kNullEntity;
    float bestDistance = std::numeric_limits<float>::max();

    // **Geometri disusun di sini, bukan di `Build`.** Adegan menggambar enam
    // puluh kali per detik dan diklik beberapa kali per menit; menyusunnya tiap
    // frame berarti membayar seluruh biayanya untuk setiap kali ia benar-benar
    // dipakai. `Sync` sendiri melewatkan pekerjaannya bila tidak ada yang
    // bergeser sejak terakhir kali.
    pickItems_.clear();
    pickItems_.reserve(pickables_.size());
    for (const Pickable& pickable : pickables_) {
        if (pickable.meshKey.empty()) {
            continue;
        }
        PickItem item;
        item.entity = pickable.entity;
        item.worldMatrix = pickable.worldMatrix;
        item.meshKey = pickable.meshKey;
        // Kunci mesh impor **adalah** jalur berkasnya; whitebox memakai GUID,
        // yang bukan jalur dan karena itu tidak punya sumber untuk dimuat.
        if (pickable.meshKey.find('/') != std::string::npos ||
            pickable.meshKey.find('\\') != std::string::npos) {
            item.sourcePath = pickable.meshKey;
        }
        pickItems_.push_back(item);
    }
    picks_.Sync(pickItems_);

    const raycast::RayHit hit = picks_.Raycast(ray.origin, ray.direction);
    if (hit) {
        best = ToEntity(hit.userData);
        // **Jaraknya dibandingkan dalam satuan yang sama** dengan jalur kotak di
        // bawah: keduanya mengukur sepanjang sinar dunia yang sama, jadi benda
        // yang geometrinya belum dimuat tetap bisa menang bila memang lebih
        // dekat — dan jawabannya tidak melompat begitu pemuatannya selesai.
        bestDistance = hit.distance;
    }

    for (const Pickable& pickable : pickables_) {
        // Yang sudah terwakili segitiga tidak diuji kotaknya lagi: kotaknya
        // selalu lebih besar daripada bentuknya, dan mengujinya kembali berarti
        // mengembalikan persis positif palsu yang baru saja dibuang.
        if (picks_.Covers(pickable.entity)) {
            continue;
        }
        // Sinar dibawa ke ruang lokal objek, bukan sebaliknya. Dengan begitu
        // ujinya tetap AABB sederhana walau objeknya diputar dan diskala, dan
        // dua objek bertumpuk terpisah tepat pada bentuk masing-masing.
        const Mat4 inverse = glm::inverse(pickable.worldMatrix);
        const Vec3 localOrigin = Vec3(inverse * Vec4(ray.origin, 1.0f));
        const Vec3 localDirection = Vec3(inverse * Vec4(ray.direction, 0.0f));

        float distance = 0.0f;
        if (!RayIntersectsAabb(localOrigin, localDirection, pickable.boundsMin, pickable.boundsMax,
                               distance)) {
            continue;
        }
        // Jarak dibandingkan dalam satuan parameter sinar yang sama untuk
        // semua objek karena arah sinar dunia tidak dinormalkan ulang per
        // objek — skala objek ikut terbawa ke localDirection, dan itu yang
        // membuat perbandingannya tetap adil.
        if (distance < bestDistance) {
            bestDistance = distance;
            best = pickable.entity;
        }
    }
    return best;
}

SceneView::SurfaceHit SceneView::RaycastSurface(const Ray& ray,
                                                std::span<const scene::Entity> ignore) const {
    SurfaceHit result;

    // Daftar yang sama dengan `Raycast`, disusun ulang di sini karena keduanya
    // dipanggil terpisah dan `Sync` sendiri melewatkan pekerjaannya bila tidak
    // ada yang bergeser.
    pickItems_.clear();
    pickItems_.reserve(pickables_.size());
    for (const Pickable& pickable : pickables_) {
        if (pickable.meshKey.empty()) {
            continue;
        }
        PickItem item;
        item.entity = pickable.entity;
        item.worldMatrix = pickable.worldMatrix;
        item.meshKey = pickable.meshKey;
        if (pickable.meshKey.find('/') != std::string::npos ||
            pickable.meshKey.find('\\') != std::string::npos) {
            item.sourcePath = pickable.meshKey;
        }
        pickItems_.push_back(item);
    }
    picks_.Sync(pickItems_);

    const raycast::RayHit hit = picks_.RaycastExcluding(
        ray.origin, ray.direction, raycast::kUnbounded, ignore);
    if (!hit) {
        return result;
    }

    result.hit = true;
    result.entity = ToEntity(hit.userData);
    result.position = hit.position;
    result.distance = hit.distance;
    // Dibalik bila menghadap searah sinar: sisi belakang sebuah permukaan tetap
    // permukaan, dan benda yang dijatuhkan ke sana harus tetap berdiri di
    // atasnya — bukan tertanam menembusnya.
    const Vec3 normal = glm::normalize(hit.normal);
    result.normal = glm::dot(normal, ray.direction) > 0.0f ? -normal : normal;
    return result;
}

std::vector<scene::Entity> SceneView::RectSelect(const Mat4& viewProjection, const Vec2& origin,
                                                 const Vec2& size, const ScreenRect& rect) const {
    std::vector<scene::Entity> result;

    for (const Pickable& pickable : pickables_) {
        Vec2 boxMin(std::numeric_limits<float>::max());
        Vec2 boxMax(std::numeric_limits<float>::lowest());
        bool anyVisible = false;

        for (int i = 0; i < 8; ++i) {
            const Vec3 local((i & 1) ? pickable.boundsMax.x : pickable.boundsMin.x,
                             (i & 2) ? pickable.boundsMax.y : pickable.boundsMin.y,
                             (i & 4) ? pickable.boundsMax.z : pickable.boundsMin.z);
            const Vec3 world = Vec3(pickable.worldMatrix * Vec4(local, 1.0f));

            Vec2 screen;
            if (!WorldToScreen(viewProjection, origin, size, world, screen)) {
                continue;  // sudut di belakang kamera, abaikan
            }
            boxMin = glm::min(boxMin, screen);
            boxMax = glm::max(boxMax, screen);
            anyVisible = true;
        }

        if (!anyVisible) {
            continue;
        }
        const bool overlaps = boxMin.x <= rect.max.x && boxMax.x >= rect.min.x &&
                              boxMin.y <= rect.max.y && boxMax.y >= rect.min.y;
        if (overlaps) {
            result.push_back(pickable.entity);
        }
    }

    // Ikon diuji sebagai titik: yang digambar memang penanda seukuran piksel
    // tetap, bukan volume, jadi tidak ada kotak yang bisa dibandingkan.
    for (const EntityIcon& icon : icons_) {
        if (!icon.pickable) {
            continue;
        }
        Vec2 screen;
        if (!WorldToScreen(viewProjection, origin, size, icon.position, screen)) {
            continue;
        }
        if (screen.x >= rect.min.x && screen.x <= rect.max.x && screen.y >= rect.min.y &&
            screen.y <= rect.max.y) {
            result.push_back(icon.entity);
        }
    }
    return result;
}

bool SceneView::BoundsOf(const std::vector<scene::Entity>& entities, Vec3& outMin,
                         Vec3& outMax) const {
    outMin = Vec3(std::numeric_limits<float>::max());
    outMax = Vec3(std::numeric_limits<float>::lowest());
    bool found = false;

    for (const Pickable& pickable : pickables_) {
        if (std::find(entities.begin(), entities.end(), pickable.entity) == entities.end()) {
            continue;
        }
        for (int i = 0; i < 8; ++i) {
            const Vec3 local((i & 1) ? pickable.boundsMax.x : pickable.boundsMin.x,
                             (i & 2) ? pickable.boundsMax.y : pickable.boundsMin.y,
                             (i & 4) ? pickable.boundsMax.z : pickable.boundsMin.z);
            const Vec3 world = Vec3(pickable.worldMatrix * Vec4(local, 1.0f));
            outMin = glm::min(outMin, world);
            outMax = glm::max(outMax, world);
        }
        found = true;
    }

    // Entity tanpa geometri tetap ikut dihitung sebagai titik, supaya "focus ke
    // seleksi" pada sebuah lampu tidak jatuh ke cabang "tidak ketemu" dan
    // melempar kamera kembali ke titik nol.
    for (const EntityIcon& icon : icons_) {
        if (std::find(entities.begin(), entities.end(), icon.entity) == entities.end()) {
            continue;
        }
        outMin = glm::min(outMin, icon.position);
        outMax = glm::max(outMax, icon.position);
        found = true;
    }
    return found;
}

/// Menyalin geometri ke cache CPU **sekali per versi**.
///
/// Tanpa penjaga versi ini `Adopt` menyalin seluruh mesh tiap frame, dan ubin
/// terrain adalah puluhan ribu vertex dikali enam puluh empat.
void SceneView::AdoptGeometry(const std::string& key, const assets::MeshData& data,
                              uint64_t version) {
    if (geometry_ == nullptr || !data.IsValid()) {
        return;
    }
    const auto found = adopted_.find(key);
    if (found != adopted_.end() && found->second == version) {
        return;
    }
    adopted_[key] = version;
    geometry_->Adopt(key, data);
}



void SceneView::BakeItems(std::vector<PickItem>& out) const {
    out.clear();
    out.reserve(bakeEntries_.size());
    for (const BakeEntry& entry : bakeEntries_) {
        PickItem item;
        item.entity = entry.entity;
        item.worldMatrix = entry.worldMatrix;
        // View ke dalam `bakeEntries_`, yang tidak berubah sampai `Build`
        // berikutnya. Pemanggil karena itu harus memakai daftarnya di dalam
        // frame yang sama — aturan yang sama seperti `Icons()` dan `Scene()`.
        item.meshKey = entry.meshKey;
        item.worldMinimum = entry.worldMinimum;
        item.worldMaximum = entry.worldMaximum;
        // Kunci mesh impor **adalah** jalur berkasnya; whitebox memakai GUID,
        // yang bukan jalur dan karena itu tidak punya sumber untuk dimuat.
        if (entry.meshKey.find('/') != std::string::npos ||
            entry.meshKey.find('\\') != std::string::npos) {
            item.sourcePath = entry.meshKey;
        }
        out.push_back(item);
    }
}

bool SceneView::GeometryBounds(Vec3& outMin, Vec3& outMax) const {
    outMin = Vec3(std::numeric_limits<float>::max());
    outMax = Vec3(std::numeric_limits<float>::lowest());
    bool found = false;

    // **`meshes_`, bukan `pickables_`, dan itu bukan pilihan gaya.** Entity yang
    // dikunci tetap digambar tetapi sengaja tidak masuk daftar pickable — dan
    // geometri yang dikunci justru yang paling mungkin ada di adegan berpanggang,
    // karena mengunci latar statis adalah cara orang menjaganya tidak terpilih
    // tak sengaja. Menghitung batas dari `pickables_` berarti panel menampilkan
    // kisi yang lebih kecil daripada kisi yang benar-benar dibangun renderer,
    // yang menyusunnya dari daftar yang sama dengan `meshes_` ini.
    for (const render::MeshInstance& instance : meshes_) {
        // Kedelapan sudutnya ditransformasi, bukan kedua ujungnya: sebuah kotak
        // yang diputar tidak lagi sejajar sumbu, dan mentransformasi min/max
        // saja menghasilkan kotak yang lebih kecil daripada isinya — kisi probe
        // lalu berhenti di dalam geometri yang harus dinaunginya.
        for (int i = 0; i < 8; ++i) {
            const Vec3 local((i & 1) ? instance.boundsMax.x : instance.boundsMin.x,
                             (i & 2) ? instance.boundsMax.y : instance.boundsMin.y,
                             (i & 4) ? instance.boundsMax.z : instance.boundsMin.z);
            const Vec3 world = Vec3(instance.transform * Vec4(local, 1.0f));
            outMin = glm::min(outMin, world);
            outMax = glm::max(outMax, world);
        }
        found = true;
    }
    return found;
}

Ray ScreenPointToRay(const Mat4& view, const Mat4& projection, const Vec2& size,
                     const Vec2& point) {
    Ray ray;
    if (size.x < 1.0f || size.y < 1.0f) {
        return ray;
    }

    // Piksel → koordinat perangkat ternormalisasi. Y tidak dibalik: proyeksi
    // kita sudah membalik [1][1] untuk Vulkan (lihat Perspective() di Math.h),
    // jadi +Y NDC sudah menunjuk ke bawah seperti koordinat layar.
    const float ndcX = (point.x / size.x) * 2.0f - 1.0f;
    const float ndcY = (point.y / size.y) * 2.0f - 1.0f;

    const Mat4 inverse = glm::inverse(projection * view);
    // Depth Vulkan [0,1]: bidang dekat di 0, bidang jauh di 1.
    Vec4 nearPoint = inverse * Vec4(ndcX, ndcY, 0.0f, 1.0f);
    Vec4 farPoint = inverse * Vec4(ndcX, ndcY, 1.0f, 1.0f);
    if (std::abs(nearPoint.w) < 1e-8f || std::abs(farPoint.w) < 1e-8f) {
        return ray;
    }
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;

    ray.origin = Vec3(nearPoint);
    ray.direction = glm::normalize(Vec3(farPoint) - Vec3(nearPoint));
    return ray;
}

bool WorldToScreen(const Mat4& viewProjection, const Vec2& origin, const Vec2& size,
                   const Vec3& world, Vec2& outScreen) {
    const Vec4 clip = viewProjection * Vec4(world, 1.0f);
    if (clip.w <= 1e-6f) {
        return false;
    }
    const Vec3 ndc = Vec3(clip) / clip.w;
    outScreen = Vec2(origin.x + (ndc.x * 0.5f + 0.5f) * size.x,
                     origin.y + (ndc.y * 0.5f + 0.5f) * size.y);
    return true;
}

}  // namespace sim::view
