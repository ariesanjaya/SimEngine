#include "Sim/Render/FrameGraph.h"

#include <algorithm>
#include <array>

namespace sim::render {
namespace {

constexpr std::array<const char*, 10> kAccessNames{
    "None",        "ColorWrite",   "DepthWrite",    "DepthRead",    "ShaderRead",
    "ShaderWrite", "TransferRead", "TransferWrite", "IndirectRead", "Present",
};

}  // namespace

const char* ToString(Access access) {
    const auto index = static_cast<std::size_t>(access);
    return index < kAccessNames.size() ? kAccessNames[index] : kAccessNames[0];
}

bool IsReadOnly(Access access) {
    switch (access) {
        case Access::DepthRead:
        case Access::ShaderRead:
        case Access::TransferRead:
        case Access::IndirectRead:
        case Access::Present:
            return true;
        default:
            return false;
    }
}

bool ResourceDesc::SameShape(const ResourceDesc& other) const {
    if (kind != other.kind) {
        return false;
    }
    if (kind == ResourceKind::Buffer) {
        return bytes == other.bytes;
    }
    return width == other.width && height == other.height && format == other.format &&
           samples == other.samples;
}

// --- pembangunan --------------------------------------------------------------

void FrameGraph::Clear() {
    passes_.clear();
    resources_.clear();
}

ResourceId FrameGraph::CreateTexture(std::string name, const ResourceDesc& desc) {
    Resource resource;
    resource.name = std::move(name);
    resource.desc = desc;
    resource.desc.kind = ResourceKind::Texture;
    resources_.push_back(std::move(resource));
    return static_cast<ResourceId>(resources_.size() - 1);
}

ResourceId FrameGraph::CreateBuffer(std::string name, uint64_t bytes) {
    Resource resource;
    resource.name = std::move(name);
    resource.desc.kind = ResourceKind::Buffer;
    resource.desc.bytes = bytes;
    resources_.push_back(std::move(resource));
    return static_cast<ResourceId>(resources_.size() - 1);
}

ResourceId FrameGraph::Import(std::string name, Access initial) {
    Resource resource;
    resource.name = std::move(name);
    resource.imported = true;
    resource.initial = initial;
    resources_.push_back(std::move(resource));
    return static_cast<ResourceId>(resources_.size() - 1);
}

PassId FrameGraph::AddPass(std::string name) {
    Pass pass;
    pass.name = std::move(name);
    passes_.push_back(std::move(pass));
    return static_cast<PassId>(passes_.size() - 1);
}

void FrameGraph::Read(PassId pass, ResourceId resource, Access access) {
    if (pass >= passes_.size() || resource >= resources_.size()) {
        return;
    }
    passes_[pass].uses.push_back(Use{resource, access, false});
}

void FrameGraph::Write(PassId pass, ResourceId resource, Access access) {
    if (pass >= passes_.size() || resource >= resources_.size()) {
        return;
    }
    passes_[pass].uses.push_back(Use{resource, access, true});
}

void FrameGraph::SetOutput(ResourceId resource, Access finalAccess) {
    if (resource >= resources_.size()) {
        return;
    }
    resources_[resource].output = true;
    resources_[resource].finalAccess = finalAccess;
}

void FrameGraph::SetSideEffect(PassId pass) {
    if (pass < passes_.size()) {
        passes_[pass].sideEffect = true;
    }
}

void FrameGraph::SetQueue(PassId pass, Queue queue) {
    if (pass < passes_.size()) {
        passes_[pass].queue = queue;
    }
}

std::string_view FrameGraph::PassName(PassId pass) const {
    return pass < passes_.size() ? std::string_view(passes_[pass].name) : std::string_view{};
}

std::string_view FrameGraph::ResourceName(ResourceId resource) const {
    return resource < resources_.size() ? std::string_view(resources_[resource].name)
                                        : std::string_view{};
}

const ResourceDesc& FrameGraph::Desc(ResourceId resource) const {
    static const ResourceDesc kFallback;
    return resource < resources_.size() ? resources_[resource].desc : kFallback;
}

bool FrameGraph::Imported(ResourceId resource) const {
    return resource < resources_.size() && resources_[resource].imported;
}

// --- kompilasi ----------------------------------------------------------------

CompiledGraph FrameGraph::Compile() const {
    CompiledGraph result;
    result.memorySlot.assign(resources_.size(), kInvalidResource);

    // Untuk tiap resource: pass mana yang menulisnya. Dipakai dua kali — menyusun
    // urutan, dan memeriksa pembacaan yang tidak punya penulis.
    std::vector<std::vector<PassId>> writers(resources_.size());
    for (PassId pass = 0; pass < passes_.size(); ++pass) {
        for (const Use& use : passes_[pass].uses) {
            if (use.write) {
                writers[use.resource].push_back(pass);
            }
        }
    }

    // 1. Pembuangan. Ditelusuri mundur dari keluaran: sebuah pass hidup kalau ia
    //    menulis sesuatu yang dibaca pass hidup lain, atau sesuatu yang menjadi
    //    keluaran. Ditelusuri mundur dan bukan dihitung maju karena yang
    //    menentukan hidup-matinya adalah "ada yang membutuhkan", dan itu
    //    pertanyaan yang arahnya memang mundur.
    std::vector<uint8_t> alive(passes_.size(), 0);
    std::vector<ResourceId> needed;
    for (ResourceId resource = 0; resource < resources_.size(); ++resource) {
        if (resources_[resource].output) {
            needed.push_back(resource);
        }
    }
    std::vector<PassId> pending;
    for (PassId pass = 0; pass < passes_.size(); ++pass) {
        if (passes_[pass].sideEffect) {
            alive[pass] = 1;
            pending.push_back(pass);
        }
    }
    std::vector<uint8_t> queued(resources_.size(), 0);
    for (const ResourceId resource : needed) {
        queued[resource] = 1;
    }
    while (!needed.empty() || !pending.empty()) {
        while (!needed.empty()) {
            const ResourceId resource = needed.back();
            needed.pop_back();
            for (const PassId writer : writers[resource]) {
                if (alive[writer] == 0) {
                    alive[writer] = 1;
                    pending.push_back(writer);
                }
            }
        }
        while (!pending.empty()) {
            const PassId pass = pending.back();
            pending.pop_back();
            for (const Use& use : passes_[pass].uses) {
                if (!use.write && queued[use.resource] == 0) {
                    queued[use.resource] = 1;
                    needed.push_back(use.resource);
                }
            }
        }
    }
    for (PassId pass = 0; pass < passes_.size(); ++pass) {
        if (alive[pass] == 0) {
            result.culled.push_back(pass);
        }
    }

    // 2. Pemeriksaan. Pass hidup yang membaca resource yang tidak pernah ditulis
    //    dan bukan impor sedang membaca isi sampah — dilaporkan sebagai galat,
    //    bukan dibiarkan menghasilkan gambar yang aneh sekali-sekali.
    for (PassId pass = 0; pass < passes_.size(); ++pass) {
        if (alive[pass] == 0) {
            continue;
        }
        for (const Use& use : passes_[pass].uses) {
            if (use.write || resources_[use.resource].imported ||
                !writers[use.resource].empty()) {
                continue;
            }
            result.error = "pass '" + passes_[pass].name + "' reads '" +
                           resources_[use.resource].name + "' which nothing writes";
            return result;
        }
    }

    // 3. Urutan. Pass hidup dijalankan menurut urutan deklarasinya, dan itu
    //    memang sudah urutan topologis: sebuah pass hanya bisa membaca resource
    //    yang penulisnya sudah dideklarasikan lebih dulu. Menyusun ulang secara
    //    topologis akan membuang satu-satunya hal yang bisa dikendalikan penulis
    //    frame — urutan yang ia tulis sendiri — demi kebebasan yang tidak
    //    dipakai siapa pun. Yang tetap diperiksa: pembacaan yang mendahului
    //    penulisnya.
    for (PassId pass = 0; pass < passes_.size(); ++pass) {
        if (alive[pass] == 0) {
            continue;
        }
        for (const Use& use : passes_[pass].uses) {
            if (use.write) {
                continue;
            }
            bool writtenBefore = resources_[use.resource].imported;
            for (const PassId writer : writers[use.resource]) {
                if (writer < pass && alive[writer] != 0) {
                    writtenBefore = true;
                    break;
                }
            }
            if (!writtenBefore) {
                result.error = "pass '" + passes_[pass].name + "' reads '" +
                               resources_[use.resource].name + "' before anything writes it";
                return result;
            }
        }
    }

    // 4. Barrier. Keadaan tiap resource ditelusuri sepanjang urutan jalan; barrier
    //    hanya dipancarkan saat keadaannya benar-benar berpindah. Dua pembacaan
    //    berurutan dengan cara yang sama tidak dipisahkan apa pun — memancarkan
    //    barrier per pass memang benar, tapi ia membuat GPU mengerjakan pass-pass
    //    itu berurutan padahal tidak ada yang menuntutnya.
    std::vector<Access> state(resources_.size(), Access::None);
    for (ResourceId resource = 0; resource < resources_.size(); ++resource) {
        state[resource] = resources_[resource].initial;
    }
    std::vector<uint32_t> firstUse(resources_.size(), 0xFFFFFFFFu);
    std::vector<uint32_t> lastUse(resources_.size(), 0);
    // Pemilik tiap resource, dan langkah tempat ia terakhir dipakai. **Grafis
    // sebagai keadaan awal**: frame dimulai dan diakhiri di sana, dan resource
    // yang belum disentuh frame ini memang ditinggalkan frame sebelumnya di
    // antrean itu.
    std::vector<Queue> owner(resources_.size(), Queue::Graphics);
    std::vector<uint32_t> ownerStep(resources_.size(), 0xFFFFFFFFu);
    // Sisi "dari" tiap perpindahan kepemilikan: (langkah pelepas, langkah
    // pengambil). Dipakai menyusun segmen sesudah seluruh pass terkumpul.
    std::vector<std::pair<uint32_t, uint32_t>> crossings;

    uint32_t step = 0;
    for (PassId pass = 0; pass < passes_.size(); ++pass) {
        if (alive[pass] == 0) {
            continue;
        }
        CompiledPass compiled;
        compiled.pass = pass;
        compiled.queue = passes_[pass].queue;
        compiled.name = passes_[pass].name;
        for (const Use& use : passes_[pass].uses) {
            // **Tulisan storage menuntut barrier walaupun keadaannya tidak
            // berpindah.** Vulkan tidak menjanjikan urutan apa pun antara dua
            // perintah pada antrean yang sama, dan dispatch tidak melewati tahap
            // fungsi-tetap yang punya urutannya sendiri seperti lampiran warna.
            // Dua dispatch berurutan yang menulis image atau buffer yang sama
            // karena itu bisa berjalan berbarengan: yang kedua membaca sebagian
            // tulisan yang pertama, dan hasilnya berubah-ubah antar-jalan tanpa
            // satu pun galat. Barrier "dari ShaderWrite ke ShaderWrite" tampak
            // kosong dan justru satu-satunya yang menutup lubang itu.
            //
            // Hanya untuk `ShaderWrite`. Lampiran warna dan depth beruntun —
            // langit, grid, opaque, transparan — memang tidak dipisahkan apa pun
            // di sini, dan itu keputusan yang berdiri sendiri: menambahkan
            // barrier di antara setiap pass adegan adalah perubahan biaya yang
            // harus diukur, bukan efek samping dari milestone compute.
            //
            // **Perpindahan antrean menuntut barrier walaupun keadaannya tidak
            // berpindah.** Yang dipindahkan bukan tata letak melainkan
            // kepemilikan keluarga antrean, dan tanpa sepasang lepas-ambil isi
            // resource itu tidak dijanjikan apa pun di sisi penerimanya.
            //
            // Hanya kalau resource-nya memang sudah dipakai frame ini. Yang
            // belum: isinya datang dari frame sebelumnya, dan satu-satunya pass
            // yang boleh memakainya di antrean lain adalah pass yang
            // menimpanya — perpindahan yang tidak dilakukan di sana tidak
            // menghilangkan apa pun.
            const bool crosses = passes_[pass].queue != owner[use.resource] &&
                                 ownerStep[use.resource] != 0xFFFFFFFFu;
            const bool moved = state[use.resource] != use.access;
            if (moved || crosses || use.access == Access::ShaderWrite) {
                Barrier barrier{use.resource, state[use.resource], use.access};
                if (crosses) {
                    barrier.fromQueue = owner[use.resource];
                    barrier.toQueue = passes_[pass].queue;
                    // Sisi pelepasnya ditempelkan ke pass yang terakhir
                    // memakainya, bukan ke pass ini: pelepasan harus berjalan
                    // di antrean pemilik lama.
                    result.order[ownerStep[use.resource]].releases.push_back(barrier);
                    crossings.emplace_back(ownerStep[use.resource], step);
                }
                compiled.barriers.push_back(barrier);
                state[use.resource] = use.access;
            }
            owner[use.resource] = passes_[pass].queue;
            ownerStep[use.resource] = step;
            if (firstUse[use.resource] == 0xFFFFFFFFu) {
                firstUse[use.resource] = step;
            }
            lastUse[use.resource] = step;
        }
        result.order.push_back(std::move(compiled));
        ++step;
    }

    // 5. Segmen. Urutan jalan dipotong setiap kali antreannya berganti, **dan**
    //    setiap kali sebuah pass harus menunggu antrean lain — penungguan
    //    berlaku di awal submit, jadi pekerjaan yang boleh jalan lebih dulu
    //    harus berada di potongan yang berbeda. Tanpa pemotongan kedua itu,
    //    tumpang tindih yang baru saja dibeli hilang di baris pertama.
    if (!result.order.empty()) {
        std::vector<uint8_t> waitsHere(result.order.size(), 0);
        for (const auto& [from, to] : crossings) {
            waitsHere[to] = 1;
        }
        std::vector<uint32_t> segmentOf(result.order.size(), 0);
        for (uint32_t at = 0; at < result.order.size(); ++at) {
            const bool queueChanged =
                result.segments.empty() || result.order[at].queue != result.segments.back().queue;
            if (queueChanged || (waitsHere[at] != 0 && result.segments.back().count > 0)) {
                Segment segment;
                segment.queue = result.order[at].queue;
                segment.first = at;
                result.segments.push_back(segment);
            }
            ++result.segments.back().count;
            segmentOf[at] = static_cast<uint32_t>(result.segments.size() - 1);
        }

        // Ketergantungan antar-segmen, lalu nilai timeline-nya. Nilainya dibagi
        // per antrean dan menaik di dalamnya: sebuah operasi signal hanya sah
        // bila nilainya lebih besar daripada nilai semaphore saat ia berjalan,
        // dan dua antrean yang berjalan bersamaan tidak menjamin urutan itu.
        std::vector<std::vector<uint32_t>> dependsOn(result.segments.size());
        for (const auto& [from, to] : crossings) {
            const uint32_t producer = segmentOf[from];
            const uint32_t consumer = segmentOf[to];
            if (producer != consumer) {
                dependsOn[consumer].push_back(producer);
            }
        }
        std::array<uint64_t, 2> next{0, 0};
        for (uint32_t at = 0; at < result.segments.size(); ++at) {
            for (const uint32_t producer : dependsOn[at]) {
                Segment& source = result.segments[producer];
                if (source.signal == 0) {
                    source.signal = ++next[static_cast<std::size_t>(source.queue)];
                }
                if (source.signal > result.segments[at].wait) {
                    result.segments[at].wait = source.signal;
                    result.segments[at].waitQueue = source.queue;
                }
            }
        }
    }

    // Keluaran dikembalikan ke keadaan yang dijanjikannya — **sesudah** pass
    // terakhir, bukan sebelumnya. Ditempelkan ke barrier pass terakhir, transisi
    // ini justru berjalan sebelum pass itu menulis, dan yang diserahkan ke ImGui
    // adalah target yang belum digambar.
    for (ResourceId resource = 0; resource < resources_.size(); ++resource) {
        if (resources_[resource].output && state[resource] != resources_[resource].finalAccess) {
            result.finalBarriers.push_back(
                Barrier{resource, state[resource], resources_[resource].finalAccess});
            state[resource] = resources_[resource].finalAccess;
        }
    }

    // 5. Alias memori. Dua transien boleh berbagi slot kalau umurnya tidak
    //    bertumpang tindih dan bentuknya sama persis.
    //
    //    Ditentukan lewat selang umur, bukan lewat kolam yang membagikan apa pun
    //    yang sedang bebas. Kolam bergantung pada urutan permintaan, jadi jumlah
    //    memori yang terpakai berubah-ubah antar-frame tanpa ada yang
    //    mengubahnya — dan "kadang kehabisan memori" adalah bug yang paling
    //    mahal dicari. Selang umur memberi jawaban yang sama untuk graph yang
    //    sama.
    struct Slot {
        ResourceDesc desc;
        uint32_t freeFrom = 0;
        bool used = false;
    };
    std::vector<Slot> slots;
    for (ResourceId resource = 0; resource < resources_.size(); ++resource) {
        if (resources_[resource].imported || firstUse[resource] == 0xFFFFFFFFu) {
            continue;
        }
        // Keluaran tidak pernah dialias-kan: isinya masih dibaca sesudah graph
        // selesai, dan slot yang dipakai ulang sesudah pemakaian terakhirnya di
        // dalam graph bukan berarti bebas di luar graph.
        const bool aliasable = !resources_[resource].output;
        int chosen = -1;
        if (aliasable) {
            for (std::size_t i = 0; i < slots.size(); ++i) {
                if (slots[i].used && slots[i].freeFrom <= firstUse[resource] &&
                    slots[i].desc.SameShape(resources_[resource].desc)) {
                    chosen = static_cast<int>(i);
                    break;
                }
            }
        }
        if (chosen < 0) {
            slots.push_back(Slot{resources_[resource].desc, 0, true});
            chosen = static_cast<int>(slots.size()) - 1;
        }
        slots[static_cast<std::size_t>(chosen)].freeFrom = lastUse[resource] + 1;
        result.memorySlot[resource] = static_cast<ResourceId>(chosen);
    }
    result.slotCount = static_cast<uint32_t>(slots.size());
    result.ok = true;
    return result;
}

}  // namespace sim::render
