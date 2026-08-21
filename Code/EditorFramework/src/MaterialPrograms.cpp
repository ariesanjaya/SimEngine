#include "Sim/Editor/MaterialPrograms.h"

#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Core/Log.h"
#include "Sim/Core/TaskPool.h"
#include "Sim/Material/MaterialGraph.h"
#include "Sim/Material/MaterialShaderModule.h"
#include "Sim/Render/IViewportRenderer.h"

#include <utility>

namespace sim::editor {

MaterialPrograms::MaterialPrograms(std::filesystem::path shaderCacheDir,
                                   std::filesystem::path shaderDir, TaskPool* tasks)
    : shaderCacheDir_(std::move(shaderCacheDir)), shaderDir_(std::move(shaderDir)),
      tasks_(tasks) {
    if (shaderCacheDir_.empty() || shaderDir_.empty()) {
        return;
    }
    const std::string identity = material::SlangCompilerIdentity();
    if (identity.empty()) {
        // Dilaporkan sekali, bukan didiamkan: tanpa slangc setiap ruas digambar
        // jalur mundur, dan yang melihatnya akan mengira materialnya yang salah
        // alih-alih toolchain-nya yang tidak ada.
        SIM_WARN("Editor", "slangc not found — meshes draw with the fallback shader");
        return;
    }
    prelude_ = material::LoadOpenPbrPrelude(shaderDir_);
    if (prelude_.empty()) {
        SIM_WARN("Editor", "openpbr.slang not found in {}", shaderDir_.string());
        return;
    }
    // Berkas yang sama persis yang di-`#include` `box.frag`. Lihat catatannya di
    // `AssembleForwardMaterialModule`.
    frameDeclarations_ = material::InlineShaderIncludes(
        shaderDir_,
        {"box_push.slang", "box_varyings.slang", "cluster_common.slang", "gi_resolve.slang"});
    if (frameDeclarations_.empty()) {
        SIM_WARN("Editor", "renderer shader headers not found in {}", shaderDir_.string());
        return;
    }
    cache_.Configure(shaderCacheDir_, identity);
    cache_.SetCompiler(material::MakeSlangCompiler());
    usable_ = true;
}

MaterialProgramRef MaterialPrograms::Request(const assets::AssetDatabase& assets,
                                             const Uuid& guid,
                                             render::IViewportRenderer& renderer,
                                             const MaterialTextureResolver& resolveTexture) {
    if (!usable_ || !guid.IsValid()) {
        return {MaterialProgramState::Failed, render::kInvalidMaterial};
    }

    Compiled ready;
    Job job;
    bool needsCompile = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(guid);
        if (found != entries_.end()) {
            Entry& entry = found->second;
            if (entry.state != MaterialProgramState::Pending || !entry.compiled) {
                return {entry.state, entry.handle};
            }
            // Sudah selesai dikompilasi dan belum diambil. Sisanya **wajib**
            // main thread: ia membuat pipeline dan descriptor set.
            //
            // **Disalin, bukan diambil.** Kalau ternyata ada teksturnya yang
            // belum di-bake, entri ini harus tetap utuh untuk frame berikutnya.
            ready = entry.result;
        } else {
            // Dicatat `Pending` **sebelum** tugasnya diantre. Tanpa itu,
            // permintaan kedua pada frame yang sama mengantre tugas kedua untuk
            // material yang sama — dua panggilan slangc untuk hasil identik.
            entries_.emplace(guid, Entry{});

            const assets::AssetRecord* record = assets.Find(guid);
            if (record == nullptr) {
                entries_[guid].state = MaterialProgramState::Failed;
                return {MaterialProgramState::Failed, render::kInvalidMaterial};
            }

            // **Graph-nya milik induk, nilainya milik instance.** Yang dirujuk
            // sebuah ruas hampir selalu `.simmatinst`; yang menyatakan node dan
            // parameternya adalah `.simmat` di atasnya. Berkas yang bukan
            // instance dibuka sebagai graph apa adanya — itu material yang
            // ditulis tangan, dan ia sah.
            //
            // Keduanya diselesaikan **di sini**, di main thread, karena hanya di
            // sini `AssetDatabase` boleh disentuh.
            const std::filesystem::path path = assets.AbsolutePath(*record);
            if (material::LoadInstanceFromFile(job.instance, path).ok) {
                job.hasInstance = true;
                const assets::AssetRecord* parent = assets.Find(job.instance.parent);
                if (parent == nullptr) {
                    entries_[guid].state = MaterialProgramState::Failed;
                    SIM_WARN("Editor", "material {} has no parent in the index", record->name);
                    return {MaterialProgramState::Failed, render::kInvalidMaterial};
                }
                job.graphPath = assets.AbsolutePath(*parent);
            } else {
                job.graphPath = path;
            }
            job.bindless = renderer.UsesBindlessMaterials();
            needsCompile = true;
        }
    }

    if (needsCompile) {
        if (tasks_ != nullptr) {
            tasks_->Submit([this, guid, job = std::move(job)]() mutable {
                Compile(guid, std::move(job));
            });
            return {MaterialProgramState::Pending, render::kInvalidMaterial};
        }
        // **Kuncinya sudah dilepas di atas.** `Compile` mengambilnya sendiri di
        // akhir, dan memanggilnya sambil masih memegangnya adalah kunci ganda —
        // yaitu editor yang berhenti sepenuhnya, bukan sebuah galat. Jalur ini
        // tidak pernah dilewati editor, jadi tidak ada yang akan menemukannya
        // di sana; yang menemukannya sebuah mutasi.
        Compile(guid, std::move(job));
        std::lock_guard<std::mutex> lock(mutex_);
        Entry& entry = entries_[guid];
        if (!entry.compiled) {
            return {entry.state, entry.handle};
        }
        ready = entry.result;
    }

    // Tekstur diselesaikan di sini dan bukan di worker: yang memegang baker
    // tekstur adalah pemanggil, dan hasilnya sebuah handle GPU.
    std::vector<render::TextureHandle> textures;
    textures.reserve(ready.textures.size());
    bool texturesReady = true;
    for (const Uuid& image : ready.textures) {
        if (!image.IsValid() || !resolveTexture) {
            textures.push_back(render::kInvalidTexture);
            continue;
        }
        const ResolvedMaterialTexture resolved = resolveTexture(image);
        texturesReady = texturesReady && resolved.ready;
        textures.push_back(resolved.handle);
    }

    // **Materialnya tidak dibangun sampai seluruh teksturnya siap.** Descriptor
    // set ditulis sekali dan tidak pernah ditinjau lagi, jadi membangunnya
    // sekarang berarti mengunci placeholder ke dalamnya selamanya — dan yang
    // terlihat bukan objek yang sedang menunggu melainkan objek magenta yang
    // tidak pernah berubah.
    //
    // Sambil menunggu, ruas itu digambar jalur mundur `box.frag`. Itu tetap
    // memperlihatkan bentuk dan warnanya; yang tertunda hanya modelnya.
    if (!texturesReady) {
        return {MaterialProgramState::Pending, render::kInvalidMaterial};
    }

    {
        // Hasil kompilasinya baru boleh dilepas sekarang.
        std::lock_guard<std::mutex> lock(mutex_);
        Entry& entry = entries_[guid];
        entry.result = {};
        entry.compiled = false;
    }

    // Kuncinya memuat hash SPIR-V-nya, bukan hanya GUID-nya. Material yang
    // disunting menghasilkan shader yang lain, dan kunci yang cuma GUID akan
    // membuat renderer dengan patuh mengembalikan pipeline yang lama.
    uint64_t hash = 1469598103934665603ull;
    for (const uint32_t word : ready.spirv) {
        hash = (hash ^ word) * 1099511628211ull;
    }
    for (const uint8_t byte : ready.parameters) {
        hash = (hash ^ byte) * 1099511628211ull;
    }
    for (const render::TextureHandle texture : textures) {
        hash = (hash ^ texture) * 1099511628211ull;
    }

    render::IViewportRenderer::MaterialProgram program;
    program.fragmentSpirv = ready.spirv;
    program.parameters = ready.parameters;
    program.textures = textures;
    program.bindless = ready.bindless;
    program.masked = ready.masked;
    const render::MaterialHandle handle =
        renderer.AcquireMaterial(guid.ToString() + ":" + std::to_string(hash), program);

    std::lock_guard<std::mutex> lock(mutex_);
    Entry& entry = entries_[guid];
    entry.handle = handle;
    entry.state = handle == render::kInvalidMaterial ? MaterialProgramState::Failed
                                                     : MaterialProgramState::Ready;
    return {entry.state, entry.handle};
}

void MaterialPrograms::Compile(Uuid guid, Job job) {
    compiles_.fetch_add(1, std::memory_order_relaxed);
    material::MaterialGraph graph;
    bool ok = material::LoadMaterialFromFile(graph, job.graphPath).ok;

    material::MaterialCompileResult compiled;
    if (ok) {
        material::MaterialCompileOptions options;
        options.bindless = job.bindless;
        compiled = material::CompileMaterial(graph, options);
        ok = compiled.ok;
    }

    material::CompileOutput fragment;
    if (ok) {
        material::ForwardMaterialOptions options;
        options.prelude = prelude_;
        options.frameDeclarations = frameDeclarations_;
        options.lobes = compiled.lobes;
        options.alphaTest = compiled.alphaTest;
        options.alphaCutoff = compiled.alphaCutoff;
        // Cache dikunci di sini dan bukan di sekitar seluruh fungsi: yang harus
        // berurutan hanyalah pembacaan dan penulisan cache-nya, sedangkan
        // penguraian graph boleh berjalan bersamaan.
        std::lock_guard<std::mutex> lock(cacheMutex_);
        fragment = cache_.Get(material::MakeForwardMaterialRequest(compiled.slang, options));
        ok = fragment.ok;
    }

    Entry entry;
    if (!ok) {
        entry.state = MaterialProgramState::Failed;
        SIM_WARN("Editor", "cannot compile material {}: {}", job.graphPath.filename().string(),
                 fragment.error.empty() ? "graph did not compile" : fragment.error);
        std::lock_guard<std::mutex> lock(mutex_);
        entries_[guid] = std::move(entry);
        return;
    }

    entry.result.masked = compiled.alphaTest;

    material::MaterialParameterBlock block;
    block.Build(graph.parameters);
    static const std::vector<material::ParameterOverride> kNoOverrides;
    block.Fill(graph.parameters, job.hasInstance ? job.instance.overrides : kNoOverrides,
               entry.result.parameters);

    // Slot yang diisi parameter diambil dari instance; yang tidak, dari node-nya
    // sendiri — pembagian yang sama yang dipegang `ResolveTextures`.
    entry.result.textures.reserve(compiled.textures.size());
    for (const material::TextureBinding& binding : compiled.textures) {
        Uuid image = binding.texture;
        if (job.hasInstance && !binding.parameter.empty()) {
            const Uuid overridden = job.instance.Texture(binding.parameter);
            if (overridden.IsValid()) {
                image = overridden;
            }
        }
        entry.result.textures.push_back(image);
    }
    entry.result.spirv = std::move(fragment.spirv);
    entry.result.bindless = job.bindless;
    entry.state = MaterialProgramState::Pending;
    entry.compiled = true;

    std::lock_guard<std::mutex> lock(mutex_);
    entries_[guid] = std::move(entry);
}

void MaterialPrograms::Invalidate(const Uuid& guid) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(guid);
}

std::size_t MaterialPrograms::PendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t pending = 0;
    for (const auto& [guid, entry] : entries_) {
        if (entry.state == MaterialProgramState::Pending) {
            ++pending;
        }
    }
    return pending;
}

}  // namespace sim::editor
