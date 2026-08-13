#include "ClipImportBackends.h"

#include "Sim/Core/Log.h"

#include <cgltf.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace sim::animation::detail {
namespace {

/// Nilai keluaran sampler ke-`key`, sudah melewati CUBICSPLINE bila perlu.
///
/// **CUBICSPLINE menyimpan tiga nilai per kunci** — tangen masuk, nilainya,
/// tangen keluar — jadi indeksnya dikali tiga dan yang diambil yang tengah.
/// Membacanya seperti LINEAR mengambil tangen sebagai nilai, dan yang terlihat
/// adalah tulang yang melompat ke tempat yang tidak masuk akal di tiap kunci.
std::size_t OutputIndex(const cgltf_animation_sampler& sampler, std::size_t key) {
    return sampler.interpolation == cgltf_interpolation_type_cubic_spline ? key * 3 + 1 : key;
}

bool ReadFloats(const cgltf_accessor* accessor, std::vector<float>& out, cgltf_size components) {
    if (accessor == nullptr) {
        return false;
    }
    out.assign(accessor->count * components, 0.0f);
    return cgltf_accessor_unpack_floats(accessor, out.data(), out.size()) != 0;
}

/// Kunci sebuah kanal, sudah dipisah menurut node yang disasarnya.
struct NodeChannels {
    const cgltf_animation_sampler* translation = nullptr;
    const cgltf_animation_sampler* rotation = nullptr;
    const cgltf_animation_sampler* scale = nullptr;
};

}  // namespace

std::vector<Clip> ImportClipsFromGltfFile(const std::filesystem::path& path, std::string& error) {
    std::vector<Clip> clips;
    error.clear();

    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path.string().c_str(), &data) != cgltf_result_success) {
        error = "not a readable glTF file";
        return clips;
    }
    if (cgltf_load_buffers(&options, data, path.string().c_str()) != cgltf_result_success) {
        cgltf_free(data);
        error = "cannot read the buffers this file points at";
        return clips;
    }

    std::vector<float> times;
    std::vector<float> values;

    for (cgltf_size a = 0; a < data->animations_count; ++a) {
        const cgltf_animation& source = data->animations[a];

        // **Dikumpulkan per node lebih dulu.** glTF menyimpan satu kanal per
        // properti per node, dalam urutan apa pun; `Clip` menyimpannya per bone.
        // Mengumpulkannya dulu juga yang membuat rotasi sebuah node bisa
        // disamakan belahannya — itu menuntut seluruh kuncinya sekaligus.
        std::unordered_map<const cgltf_node*, NodeChannels> perNode;
        for (cgltf_size c = 0; c < source.channels_count; ++c) {
            const cgltf_animation_channel& channel = source.channels[c];
            if (channel.target_node == nullptr || channel.sampler == nullptr) {
                continue;
            }
            NodeChannels& entry = perNode[channel.target_node];
            switch (channel.target_path) {
                case cgltf_animation_path_type_translation:
                    entry.translation = channel.sampler;
                    break;
                case cgltf_animation_path_type_rotation:
                    entry.rotation = channel.sampler;
                    break;
                case cgltf_animation_path_type_scale:
                    entry.scale = channel.sampler;
                    break;
                default:
                    // `weights` menganimasikan morph target, dan mesin ini belum
                    // punya tempat untuk menyimpannya.
                    break;
            }
        }

        // Waktu paling awal di seluruh kanal. Klip mulai dari nol, bukan dari
        // tempatnya kebetulan berada di timeline berkasnya.
        float begin = 0.0f;
        bool haveBegin = false;
        for (const auto& [node, channels] : perNode) {
            for (const cgltf_animation_sampler* sampler :
                 {channels.translation, channels.rotation, channels.scale}) {
                if (sampler == nullptr || sampler->input == nullptr ||
                    !sampler->input->has_min) {
                    continue;
                }
                const auto first = static_cast<float>(sampler->input->min[0]);
                begin = haveBegin ? std::min(begin, first) : first;
                haveBegin = true;
            }
        }

        Clip clip;
        clip.name = source.name != nullptr ? source.name : "";
        // glTF tidak menyebut laju frame di mana pun — kuncinya berwaktu detik,
        // bukan bernomor frame. Angka ini hanya keterangan untuk penyunting.
        clip.frameRate = 30.0f;
        clip.looping = true;
        float duration = 0.0f;

        for (const auto& [node, channels] : perNode) {
            const std::string bone =
                node->name != nullptr && node->name[0] != '\0' ? node->name : std::string{};
            if (bone.empty()) {
                // Track diikat lewat nama; node tanpa nama tidak akan pernah
                // menemukan bone-nya.
                continue;
            }

            // --- translasi dan skala: kanal skalar apa adanya ------------------
            const auto scalarChannels = [&](const cgltf_animation_sampler* sampler,
                                            Channel first) {
                if (sampler == nullptr || sampler->input == nullptr ||
                    sampler->output == nullptr) {
                    return;
                }
                if (!ReadFloats(sampler->input, times, 1)) {
                    return;
                }
                std::vector<float> output;
                if (!ReadFloats(sampler->output, output, 3)) {
                    return;
                }
                for (float& time : times) {
                    time -= begin;
                    duration = std::max(duration, time);
                }
                for (int component = 0; component < 3; ++component) {
                    values.assign(times.size(), 0.0f);
                    for (std::size_t k = 0; k < times.size(); ++k) {
                        const std::size_t at = OutputIndex(*sampler, k) * 3 +
                                               static_cast<std::size_t>(component);
                        if (at < output.size()) {
                            values[k] = output[at];
                        }
                    }
                    AddScalarChannel(
                        clip, bone,
                        static_cast<Channel>(static_cast<int>(first) + component), times, values);
                }
            };
            scalarChannels(channels.translation, Channel::TranslationX);
            scalarChannels(channels.scale, Channel::ScaleX);

            // --- rotasi: track kuaternion tersendiri ---------------------------
            if (channels.rotation != nullptr && channels.rotation->input != nullptr &&
                channels.rotation->output != nullptr &&
                ReadFloats(channels.rotation->input, times, 1)) {
                std::vector<float> output;
                if (ReadFloats(channels.rotation->output, output, 4)) {
                    std::vector<SampledFrame> frames(times.size());
                    for (std::size_t k = 0; k < times.size(); ++k) {
                        frames[k].time = times[k] - begin;
                        duration = std::max(duration, frames[k].time);
                        const std::size_t at = OutputIndex(*channels.rotation, k) * 4;
                        if (at + 3 < output.size()) {
                            // glTF menyimpan x, y, z, w; konstruktor glm
                            // menerima w lebih dulu.
                            frames[k].rotation = glm::normalize(Quat(
                                output[at + 3], output[at], output[at + 1], output[at + 2]));
                        }
                    }
                    AlignHemisphere(frames);
                    const int track = clip.EnsureRotationTrack(bone);
                    for (const SampledFrame& frame : frames) {
                        clip.RotationTrackAt(track).AddKey(
                            RotationKey{frame.time, frame.rotation});
                    }
                }
            }
        }

        clip.duration = std::max(duration, 1e-6f);
        if (clip.TrackCount() == 0 && clip.RotationTrackCount() == 0) {
            continue;
        }
        clips.push_back(std::move(clip));
    }
    cgltf_free(data);

    if (clips.size() == 1) {
        clips.front().name = path.stem().string();
    }
    if (clips.empty()) {
        error = "no animation in this file animates a node";
    }
    return clips;
}

}  // namespace sim::animation::detail
