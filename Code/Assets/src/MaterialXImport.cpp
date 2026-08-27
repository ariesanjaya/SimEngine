#include "Sim/Assets/MaterialXImport.h"

#include "Sim/Core/Log.h"

#include <algorithm>
#include <cctype>
#include <system_error>

#if SIM_WITH_MATERIALX
#include <MaterialXCore/Document.h>
#include <MaterialXCore/Material.h>
#include <MaterialXFormat/XmlIo.h>
#endif

namespace sim::assets {
namespace {

/// Nama yang dibandingkan tanpa peduli besar-kecil huruf maupun spasi di
/// ujungnya. Nama material melewati DCC, eksportir, dan penulis dokumen; yang
/// sampai kerap berbeda hanya pada hal-hal itu.
std::string NormalizeName(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    const auto first = out.find_first_not_of(" \t");
    const auto last = out.find_last_not_of(" \t");
    if (first == std::string::npos) {
        return {};
    }
    return out.substr(first, last - first + 1);
}

#if SIM_WITH_MATERIALX

namespace mx = MaterialX;

/// Kategori node yang **hanya meneruskan** sebuah gambar, beserta nama input
/// yang dilewatinya.
///
/// Ada karena satu tekstur jarang tersambung langsung: peta normal selalu lewat
/// `normalmap`, dan gambar satu-kanal kerap lewat `convert`. Keduanya tidak
/// mengubah gambarnya menjadi sesuatu yang mesin ini tidak bisa lakukan sendiri
/// — jadi menembusnya bukan menebak, melainkan mengenali dua node yang memang
/// tidak menambahkan apa-apa.
struct PassThrough {
    const char* category;
    const char* input;
};
constexpr PassThrough kPassThrough[] = {{"normalmap", "in"}, {"convert", "in"}};

Vec3 ToVec3(const mx::Color3& color) {
    return Vec3(color[0], color[1], color[2]);
}

/// Nilai sebuah input, ditembus sampai ke `interfacename`-nya.
///
/// Input di dalam nodegraph kerap tidak membawa nilainya sendiri melainkan
/// menunjuk input milik graph-nya. Berhenti di yang pertama berarti membaca
/// "tidak ada nilai" untuk input yang jelas-jelas punya.
mx::ValuePtr ResolvedValue(const mx::InputPtr& input) {
    if (!input) {
        return nullptr;
    }
    if (input->hasValue()) {
        return input->getValue();
    }
    if (const mx::InputPtr interface = input->getInterfaceInput()) {
        if (interface->hasValue()) {
            return interface->getValue();
        }
    }
    return nullptr;
}

/// Jalur berkas gambar yang mengemudikan sebuah input, bila memang sebuah
/// gambar yang mengemudikannya.
///
/// Mengembalikan kosong untuk input yang dikemudikan hal lain — sebuah
/// `noise2d`, sebuah `mix`, sebuah nodegraph utuh. Itu bukan kegagalan
/// melainkan batas yang disengaja: mesin ini menyambungkan satu tekstur ke satu
/// saluran, dan memanggang graph orang lain menjadi berkas baru adalah
/// pekerjaan yang tidak diminta siapa pun. Yang dilewati dicatat pemanggilnya.
std::string ImagePath(const mx::InputPtr& input, std::string& drivenBy) {
    mx::NodePtr node = input ? input->getConnectedNode() : nullptr;
    // Batas kedalaman: dokumen boleh saja memuat rantai `convert` yang
    // menunjuk dirinya sendiri, dan penelusuran tanpa batas menggantung
    // importir alih-alih menolak berkasnya.
    for (int depth = 0; node && depth < 8; ++depth) {
        const std::string& category = node->getCategory();
        if (category == "image" || category == "tiledimage") {
            const mx::InputPtr file = node->getInput("file");
            std::string path = file ? file->getValueString() : std::string{};
            if (path.empty() && file) {
                if (const mx::InputPtr interface = file->getInterfaceInput()) {
                    path = interface->getValueString();
                }
            }
            std::replace(path.begin(), path.end(), '\\', '/');
            return path;
        }
        const PassThrough* next = nullptr;
        for (const PassThrough& candidate : kPassThrough) {
            if (category == candidate.category) {
                next = &candidate;
                break;
            }
        }
        if (next == nullptr) {
            drivenBy = category;
            return {};
        }
        node = node->getConnectedNode(next->input);
    }
    if (node) {
        drivenBy = node->getCategory();
    }
    return {};
}

/// Apakah sebuah input dikemudikan sesuatu alih-alih membawa nilai sendiri.
bool IsConnected(const mx::InputPtr& input) {
    return input && (input->hasNodeName() || input->hasNodeGraphString() ||
                     input->hasInterfaceName() || input->hasOutputString());
}

/// Membaca satu input `.mtlx` ke sebuah medan `OpenPbrMaterial`.
///
/// `texture` boleh null — itu berarti saluran ini tidak punya slot tekstur di
/// induknya, dan gambar yang mengemudikannya karena itu tidak bisa dibawa.
/// Bedanya dicatat: sebuah input yang hilang diam-diam adalah persis bentuk
/// kesalahan yang tidak akan dicari orang.
template <typename T, typename Assign>
void ReadInput(const mx::NodePtr& shader, const char* mtlxName, OpenPbrTexture* texture,
               MaterialXDocument& document, std::string_view material, Assign assign) {
    const mx::InputPtr input = shader->getInput(mtlxName);
    if (!input) {
        return;  // Tidak disebut dokumennya: bawaan pin yang berlaku.
    }
    if (const mx::ValuePtr value = ResolvedValue(input)) {
        if (value->isA<T>()) {
            assign(value->asA<T>(), false);
            return;
        }
    }
    if (!IsConnected(input)) {
        return;
    }

    std::string drivenBy;
    const std::string path = ImagePath(input, drivenBy);
    if (!path.empty() && texture != nullptr) {
        texture->path = path;
        // **Skalarnya menjadi identitas perkalian, bukan dibiarkan bawaan.**
        // Induknya menyusun saluran ini sebagai `skalar × tekstur`; membiarkan
        // `specular_roughness` di 0,3 berarti gambar kekasarannya diredam
        // menjadi sepertiganya, dan yang terlihat cuma permukaan yang terlalu
        // mengkilap di mana-mana.
        assign(T(1.0f), true);
        return;
    }
    if (!path.empty()) {
        document.notes.push_back(std::string(material) + ": " + mtlxName +
                                 " memakai tekstur " + path +
                                 ", dan saluran itu tidak punya slot tekstur di induk impor");
        return;
    }
    document.notes.push_back(std::string(material) + ": " + mtlxName + " dikemudikan node " +
                             (drivenBy.empty() ? "yang tidak dikenali" : drivenBy) +
                             ", jadi yang dipakai nilai bawaannya");
}

void ReadScalar(const mx::NodePtr& shader, const char* mtlxName, float& target,
                OpenPbrTexture* texture, MaterialXDocument& document, std::string_view material) {
    ReadInput<float>(shader, mtlxName, texture, document, material,
                     [&target](float value, bool) { target = value; });
}

void ReadColor(const mx::NodePtr& shader, const char* mtlxName, Vec3& target,
               OpenPbrTexture* texture, MaterialXDocument& document, std::string_view material) {
    ReadInput<mx::Color3>(shader, mtlxName, texture, document, material,
                          [&target](const mx::Color3& value, bool identity) {
                              target = identity ? Vec3(1.0f) : ToVec3(value);
                          });
}

/// Menerjemahkan satu node `open_pbr_surface` menjadi `OpenPbrMaterial`.
///
/// Pemetaannya satu-ke-satu dan sengaja ditulis berderet: yang membacanya
/// sedang memeriksa apakah `coat_ior` sampai ke `coatIor`, dan deretan ini
/// jawaban yang bisa dibaca sekali lihat.
OpenPbrMaterial ReadOpenPbrSurface(const mx::NodePtr& shader, std::string_view name,
                                   MaterialXDocument& document) {
    OpenPbrMaterial material;
    material.name = std::string(name);

    ReadScalar(shader, "base_weight", material.baseWeight, nullptr, document, name);
    ReadColor(shader, "base_color", material.baseColor, &material.baseColorTexture, document, name);
    ReadScalar(shader, "base_metalness", material.baseMetalness, &material.baseMetalnessTexture,
               document, name);
    ReadScalar(shader, "base_diffuse_roughness", material.baseDiffuseRoughness, nullptr, document,
               name);

    ReadScalar(shader, "specular_weight", material.specularWeight, nullptr, document, name);
    ReadColor(shader, "specular_color", material.specularColor, nullptr, document, name);
    ReadScalar(shader, "specular_roughness", material.specularRoughness,
               &material.specularRoughnessTexture, document, name);
    ReadScalar(shader, "specular_roughness_anisotropy", material.specularRoughnessAnisotropy,
               nullptr, document, name);
    ReadScalar(shader, "specular_ior", material.specularIor, nullptr, document, name);

    ReadScalar(shader, "coat_weight", material.coatWeight, nullptr, document, name);
    ReadColor(shader, "coat_color", material.coatColor, nullptr, document, name);
    ReadScalar(shader, "coat_roughness", material.coatRoughness, nullptr, document, name);
    ReadScalar(shader, "coat_roughness_anisotropy", material.coatRoughnessAnisotropy, nullptr,
               document, name);
    ReadScalar(shader, "coat_ior", material.coatIor, nullptr, document, name);
    ReadScalar(shader, "coat_darkening", material.coatDarkening, nullptr, document, name);

    ReadScalar(shader, "fuzz_weight", material.fuzzWeight, nullptr, document, name);
    ReadColor(shader, "fuzz_color", material.fuzzColor, nullptr, document, name);
    ReadScalar(shader, "fuzz_roughness", material.fuzzRoughness, nullptr, document, name);

    // **Emisi dikalikan di sini, sekali.** OpenPBR memisahkan luminansi
    // (bersatuan nit) dari warnanya; pin mesin ini satu float3. Satuannya
    // sendiri belum dikalibrasi terhadap satuan lampu di sini — lihat catatan
    // di docs/PLAN-MATERIALX.md — jadi yang dilakukan adalah perkalian apa
    // adanya, bukan sebuah faktor karangan yang tidak bisa dijelaskan siapa pun.
    float luminance = 0.0f;
    Vec3 emissionColor(1.0f);
    ReadScalar(shader, "emission_luminance", luminance, nullptr, document, name);
    ReadColor(shader, "emission_color", emissionColor, &material.emissiveTexture, document, name);
    material.emissive = emissionColor * luminance;

    ReadScalar(shader, "geometry_opacity", material.opacity, &material.opacityTexture, document,
               name);

    // **Lapisan yang mesin ini memang belum punya, disebut namanya.**
    // Transmission, subsurface, dan thin film ada di OpenPBR dan tidak ada di
    // `openpbr.slang` — jadi material kaca dari Max masuk sebagai permukaan
    // yang buram. Itu batas yang sah, dan yang tidak sah adalah membiarkannya
    // lewat tanpa suara: yang mengimpornya akan mencari kesalahannya di
    // pencahayaan, di tekstur, dan di eksportirnya, karena tidak ada satu pun
    // baris yang menyebut bahwa lapisannya memang dibuang di sini.
    //
    // Dibaca langsung, bukan lewat `ReadScalar`: yang dicari di sini bukan
    // nilainya melainkan "dipakai atau tidak", dan bobot yang **dikemudikan
    // tekstur** juga berarti dipakai — sebuah pertanyaan yang tidak dijawab
    // angka.
    for (const char* layer : {"transmission_weight", "subsurface_weight", "thin_film_weight"}) {
        const mx::InputPtr input = shader->getInput(layer);
        if (!input) {
            continue;
        }
        const mx::ValuePtr value = ResolvedValue(input);
        if (value && value->isA<float>()) {
            if (value->asA<float>() != 0.0f) {
                document.notes.push_back(std::string(name) + ": " + layer + " = " +
                                         value->getValueString() +
                                         ", dan lapisan itu belum ada di openpbr.slang — "
                                         "materialnya masuk tanpa lapisan tersebut");
            }
            continue;
        }
        if (IsConnected(input)) {
            document.notes.push_back(std::string(name) + ": " + layer +
                                     " dikemudikan sesuatu, dan lapisan itu belum ada di "
                                     "openpbr.slang — materialnya masuk tanpa lapisan tersebut");
        }
    }

    // Peta normal: `geometry_normal` yang dikemudikan sebuah `normalmap`.
    // Bawaannya `defaultgeomprop="Nworld"` — yaitu "normal mesh apa adanya" —
    // dan itu keadaan yang tidak perlu dibawa ke mana-mana.
    if (const mx::InputPtr normal = shader->getInput("geometry_normal"); IsConnected(normal)) {
        std::string drivenBy;
        const std::string path = ImagePath(normal, drivenBy);
        if (!path.empty()) {
            material.normalTexture.path = path;
        } else {
            document.notes.push_back(std::string(name) + ": geometry_normal dikemudikan node " +
                                     (drivenBy.empty() ? "yang tidak dikenali" : drivenBy) +
                                     ", jadi peta normalnya tidak ikut");
        }
    }
    return material;
}

/// Shader yang dikenali, beserta apa yang harus dikatakan tentang yang tidak.
bool IsOpenPbr(const mx::NodePtr& shader) {
    return shader && shader->getCategory() == "open_pbr_surface";
}

#endif  // SIM_WITH_MATERIALX

}  // namespace

bool MaterialXAvailable() {
#if SIM_WITH_MATERIALX
    return true;
#else
    return false;
#endif
}

#if SIM_WITH_MATERIALX

bool LoadMaterialXDocument(const std::filesystem::path& path, MaterialXDocument& out,
                           std::string& error) {
    out.materials.clear();
    out.notes.clear();
    error.clear();

    mx::DocumentPtr document = mx::createDocument();
    try {
        // **XInclude dibiarkan menyala, dan pencariannya dibatasi ke folder
        // dokumennya.** Dokumen `.mtlx` boleh menarik berkas lain, dan yang
        // ditarik hampir selalu tetangganya. Membiarkan pencariannya kosong
        // membuat setiap include gagal; melebarkannya ke seluruh sistem berkas
        // membuat sebuah dokumen bisa menyebut berkas mana pun di mesin ini.
        mx::FileSearchPath search(path.parent_path().string());
        mx::readFromXmlFile(document, path.string(), search);
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }

    // Material yang dibungkus `surfacematerial` — bentuk yang ditulis hampir
    // setiap eksportir, dan yang namanya dilihat artis di DCC-nya.
    for (const mx::NodePtr& materialNode : document->getMaterialNodes()) {
        for (const mx::NodePtr& shader : mx::getShaderNodes(materialNode)) {
            if (!IsOpenPbr(shader)) {
                out.notes.push_back(materialNode->getName() + ": shader-nya " +
                                    shader->getCategory() +
                                    ", dan yang dibaca importir ini open_pbr_surface");
                continue;
            }
            out.materials.push_back(ReadOpenPbrSurface(shader, materialNode->getName(), out));
        }
    }

    // Dokumen yang hanya berisi shader tanpa pembungkusnya tetap dibaca, dan
    // namanya lalu nama shader itu. Yang menulisnya biasanya alat, bukan DCC —
    // dan menolaknya berarti menolak berkas yang isinya lengkap hanya karena
    // satu elemen pembungkus tidak ditulis.
    if (out.materials.empty()) {
        for (const mx::NodePtr& node : document->getNodes()) {
            if (IsOpenPbr(node)) {
                out.materials.push_back(ReadOpenPbrSurface(node, node->getName(), out));
            }
        }
    }

    if (out.materials.empty() && out.notes.empty()) {
        out.notes.push_back("dokumen ini tidak memuat satu pun node open_pbr_surface");
    }
    return true;
}

#else

bool LoadMaterialXDocument(const std::filesystem::path& path, MaterialXDocument& out,
                           std::string& error) {
    out.materials.clear();
    out.notes.clear();
    error = "impor MaterialX tidak ikut dibangun (SIM_WITH_MATERIALX=OFF), jadi " +
            path.filename().string() + " tidak bisa dibaca";
    return false;
}

#endif  // SIM_WITH_MATERIALX

namespace {

/// Dokumen `.mtlx` yang pantas dicoba untuk sebuah berkas mesh, berurutan.
std::vector<std::filesystem::path> CandidateDocuments(const std::filesystem::path& meshPath,
                                                      const std::vector<std::string>& hints) {
    std::vector<std::filesystem::path> candidates;
    const std::filesystem::path folder = meshPath.parent_path();
    std::error_code code;

    for (const std::string& hint : hints) {
        if (hint.empty()) {
            continue;
        }
        std::filesystem::path resolved = std::filesystem::path(hint);
        if (resolved.is_relative()) {
            resolved = folder / resolved;
        }
        if (std::filesystem::is_regular_file(resolved, code)) {
            candidates.push_back(resolved);
        }
    }

    std::filesystem::path sidecar = meshPath;
    sidecar.replace_extension(".mtlx");
    if (std::filesystem::is_regular_file(sidecar, code)) {
        candidates.push_back(sidecar);
    }

    // Satu-satunya `.mtlx` di folder itu. Dihitung dulu, dipakai belakangan:
    // dua berkas berarti tidak ada yang bisa ditebak.
    std::filesystem::path lone;
    int count = 0;
    if (std::filesystem::is_directory(folder, code) || folder.empty()) {
        const std::filesystem::path scan = folder.empty() ? std::filesystem::path(".") : folder;
        for (const auto& entry : std::filesystem::directory_iterator(scan, code)) {
            if (entry.is_regular_file(code) && entry.path().extension() == ".mtlx") {
                lone = entry.path();
                ++count;
            }
        }
    }
    if (count == 1) {
        candidates.push_back(lone);
    }

    // Kandidat yang sama boleh muncul lewat dua jalan; yang kedua dibuang.
    std::vector<std::filesystem::path> unique;
    for (const std::filesystem::path& candidate : candidates) {
        const std::filesystem::path normal = std::filesystem::weakly_canonical(candidate, code);
        const std::filesystem::path& key = code ? candidate : normal;
        if (std::find(unique.begin(), unique.end(), key) == unique.end()) {
            unique.push_back(key);
        }
    }
    return unique;
}

}  // namespace

std::size_t ApplyMaterialX(MeshData& mesh, const std::filesystem::path& meshPath,
                           const std::vector<std::string>& hints) {
    if (mesh.materials.empty()) {
        return 0;
    }
    const std::vector<std::filesystem::path> candidates = CandidateDocuments(meshPath, hints);
    if (candidates.empty()) {
        return 0;
    }
    if (!MaterialXAvailable()) {
        SIM_WARN("Assets", "{} has a MaterialX document next to it, but MaterialX support is "
                           "not built (SIM_WITH_MATERIALX=OFF)",
                 meshPath.filename().string());
        return 0;
    }

    MaterialXDocument document;
    std::filesystem::path used;
    for (const std::filesystem::path& candidate : candidates) {
        std::string error;
        if (LoadMaterialXDocument(candidate, document, error) && !document.materials.empty()) {
            used = candidate;
            break;
        }
        if (!error.empty()) {
            SIM_WARN("Assets", "cannot read {}: {}", candidate.filename().string(), error);
        }
    }
    if (used.empty()) {
        return 0;
    }

    for (const std::string& note : document.notes) {
        SIM_WARN("Assets", "{}: {}", used.filename().string(), note);
    }

    // Indeks nama → material, dibangun sekali. Nama yang sama muncul dua kali
    // di satu dokumen adalah dokumen yang ambigu; yang pertama menang, dan itu
    // urutan yang sama dengan yang dilihat siapa pun yang membuka berkasnya.
    std::vector<std::pair<std::string, const OpenPbrMaterial*>> byName;
    byName.reserve(document.materials.size());
    for (const OpenPbrMaterial& material : document.materials) {
        byName.emplace_back(NormalizeName(material.name), &material);
    }

    std::size_t matched = 0;
    for (MeshMaterial& material : mesh.materials) {
        const std::string key = NormalizeName(material.name);
        const OpenPbrMaterial* found = nullptr;
        for (const auto& [name, candidate] : byName) {
            if (name == key && !key.empty()) {
                found = candidate;
                break;
            }
        }
        // **Satu lawan satu dipasangkan walau namanya berbeda.** Berkas mesh
        // bermaterial tunggal dan dokumen bermaterial tunggal tidak punya
        // pasangan lain yang mungkin, dan nama material memang kerap berganti
        // saat melewati eksportir. Dua lawan dua tidak: di sana menebak berarti
        // separuh kemungkinan memasang material yang salah.
        if (found == nullptr && mesh.materials.size() == 1 && document.materials.size() == 1) {
            found = &document.materials.front();
        }
        if (found == nullptr) {
            SIM_WARN("Assets", "{}: material \"{}\" has no match in {}",
                     meshPath.filename().string(), material.name, used.filename().string());
            continue;
        }

        OpenPbrMaterial applied = *found;
        // Namanya tetap nama material di berkas mesh: itu yang dipakai
        // memberi nama berkas `.simmatinst`, dan yang dicari orang di Asset
        // Browser adalah nama yang dilihatnya di DCC.
        applied.name = material.name.empty() ? applied.name : material.name;
        ProjectOpenPbrToFlat(applied, material);
        material.openPbr = std::move(applied);
        ++matched;
    }

    SIM_INFO("Assets", "{}: {} of {} materials read from {}", meshPath.filename().string(),
             matched, mesh.materials.size(), used.filename().string());
    return matched;
}

}  // namespace sim::assets
