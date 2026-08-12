#include "Sim/Animation/ClipImport.h"

#include "Sim/Core/Log.h"

#include <ufbx.h>

#include <algorithm>
#include <cmath>

namespace sim::animation {
namespace {

Vec3 ToVec3(const ufbx_vec3& v) {
    return Vec3(static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z));
}

Quat ToQuat(const ufbx_quat& q) {
    // ufbx menyimpan x, y, z, w; konstruktor glm menerima w lebih dulu.
    return Quat(static_cast<float>(q.w), static_cast<float>(q.x), static_cast<float>(q.y),
                static_cast<float>(q.z));
}

/// Benar kalau seluruh kunci sama persis — kanal yang tidak menganimasikan apa
/// pun. `ufbx` sudah menandainya, tapi tandanya konservatif untuk sebagian
/// berkas, dan kanal tetap yang lolos adalah proporsi rig sumber yang ikut
/// terbawa (lihat `AddChannel`).
bool IsConstant(const ufbx_baked_vec3_list& keys, int component) {
    if (keys.count == 0) {
        return true;
    }
    const double first = (&keys.data[0].value.x)[component];
    for (std::size_t i = 1; i < keys.count; ++i) {
        if ((&keys.data[i].value.x)[component] != first) {
            return false;
        }
    }
    return true;
}

/// Menambahkan sebuah kanal skalar dari deret kunci yang sudah dipanggang.
///
/// **Kanal yang tetap sepanjang klip tidak diimpor sama sekali**, dan itu bukan
/// penghematan tempat melainkan syarat agar klipnya bisa dipakai rig lain.
///
/// Pada animasi rangka, satu-satunya bone yang translasinya benar-benar
/// bergerak lazimnya adalah root/pinggul; translasi bone lain tetap, dan
/// nilainya adalah **panjang tulang rig yang mengekspor klip itu**. Mengimpornya
/// berarti memanggang proporsi rig sumber ke dalam klipnya, dan memasangnya ke
/// rig lain akan meregangkan rig itu menjadi proporsi si sumber.
///
/// Terukur: `Defeated.fbx` dan `Y Bot.fbx` sama-sama rig Mixamo bernama sama,
/// tapi Spine2-nya 0,09322 m lawan 0,13459 m. Dengan kanal tetap ikut diimpor,
/// memasang klip Defeated ke Y Bot memendekkan tulang itu **30,7%**; tanpa
/// kanal tetap, bone yang tidak dianimasikan tinggal di bind pose rig tujuan —
/// yaitu persis arti retargeting yang dijanjikan `RetargetMap`: sudut yang sama,
/// bukan titik yang sama.
///
/// **Interpolasinya linear, bukan bezier.** Bezier adalah bawaan `CurveKey`
/// karena itu yang diinginkan kurva yang ditulis tangan; kunci yang dipanggang
/// adalah cuplikan rapat pada laju tetap, dan bezier bertangen nol di antara
/// dua cuplikan menahan nilainya lalu meloncat — gerak yang tersendat pada klip
/// yang di berkasnya mulus.
void AddChannel(Clip& clip, const std::string& bone, Channel channel,
                const ufbx_baked_vec3_list& keys, int component, bool constant,
                double timeBegin) {
    if (keys.count == 0 || constant || IsConstant(keys, component)) {
        return;
    }
    const int track = clip.EnsureTrack(bone, channel);
    Curve& curve = clip.TrackAt(track).curve;
    for (std::size_t i = 0; i < keys.count; ++i) {
        CurveKey out;
        out.time = static_cast<float>(keys.data[i].time - timeBegin);
        out.value = ToVec3(keys.data[i].value)[component];
        out.interpolation = Interpolation::Linear;
        curve.AddKey(out);
    }
}

/// Benar kalau seluruh kunci rotasi sama persis — track yang tidak menganimasikan
/// apa pun tetap berongkos satu pencuplikan per bone per frame.
bool RotationIsConstant(const ufbx_baked_quat_list& keys) {
    for (std::size_t i = 1; i < keys.count; ++i) {
        const ufbx_quat& a = keys.data[0].value;
        const ufbx_quat& b = keys.data[i].value;
        if (a.x != b.x || a.y != b.y || a.z != b.z || a.w != b.w) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::vector<Clip> ImportClipsFromFbx(const std::filesystem::path& path, std::string& error) {
    std::vector<Clip> clips;
    error.clear();
    std::error_code exists;
    if (path.empty() || !std::filesystem::exists(path, exists)) {
        error = "file not found";
        return clips;
    }

    ufbx_load_opts options{};
    options.target_axes = ufbx_axes_right_handed_y_up;
    options.target_unit_meters = 1.0f;
    // **`MODIFY_GEOMETRY`, dan ini BUKAN mode yang dipakai `LoadMesh`.**
    //
    // Yang membedakan ketiga mode adalah di mana konversi satuan disimpan, dan
    // hanya yang ini menghasilkan transform lokal yang benar begitu dirangkai
    // induk-ke-anak — yaitu persis yang dilakukan `Pose::ComputeGlobal`. Terukur
    // pada rig Mixamo, dengan panjang tulang sebagai patokan (ia tidak boleh
    // berubah apa pun posenya):
    //
    // | mode                | kunci skala | selisih panjang tulang terbesar |
    // | ------------------- | ----------- | ------------------------------- |
    // | `TRANSFORM_ROOT`    | 1,0         | **9900%** — sentimeter, 100x     |
    // | `ADJUST_TRANSFORMS` | 0,01        | **100%** — 0,01 menumpuk tiap    |
    // |                     |             | tingkat, rangkanya runtuh        |
    // | `MODIFY_GEOMETRY`   | 1,0         | **0,0000%**                      |
    //
    // `LoadMesh` memakai `TRANSFORM_ROOT` dan benar, karena ia mengambil
    // `node_to_world` — matriks yang sudah memuat seluruh rantainya termasuk
    // konversi di node root. Yang di sini mengambil transform lokal, dan di sana
    // konversi itu belum ikut. Dua importir dengan mode berbeda karena keduanya
    // memang membaca hal yang berbeda dari berkas yang sama.
    options.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;

    ufbx_error loadError;
    ufbx_scene* scene = ufbx_load_file(path.string().c_str(), &options, &loadError);
    if (scene == nullptr) {
        error = loadError.description.data != nullptr ? loadError.description.data
                                                      : "cannot read file";
        return clips;
    }

    const float frameRate = scene->settings.frames_per_second > 1.0
                                ? static_cast<float>(scene->settings.frames_per_second)
                                : 30.0f;

    for (std::size_t stackIndex = 0; stackIndex < scene->anim_stacks.count; ++stackIndex) {
        const ufbx_anim_stack* stack = scene->anim_stacks.data[stackIndex];
        if (stack == nullptr || stack->layers.count == 0) {
            continue;
        }

        ufbx_bake_opts bakeOptions{};
        // Klip mulai dari nol, bukan dari waktu tempatnya kebetulan berada di
        // timeline berkasnya. Take yang mulai di detik ke-30 akan tampak sebagai
        // klip yang diam selama 30 detik lebih dulu.
        bakeOptions.trim_start_time = true;

        ufbx_error bakeError;
        ufbx_baked_anim* baked = ufbx_bake_anim(scene, stack->anim, &bakeOptions, &bakeError);
        if (baked == nullptr) {
            SIM_WARN("Animation", "cannot bake '{}' in {}: {}", stack->name.data, path.string(),
                     bakeError.description.data != nullptr ? bakeError.description.data : "?");
            continue;
        }

        Clip clip;
        // **Dinamai menurut berkasnya bila hanya ada satu take yang berisi.**
        // Take di berkas Mixamo bernama "mixamo.com", yang tidak memberi tahu
        // apa pun; nama berkasnya justru yang dipilih orang — "Running.fbx".
        // Beberapa take berisi berarti namanya harus membedakan, jadi di sana
        // nama take yang dipakai.
        clip.name = stack->name.data != nullptr ? stack->name.data : "";
        clip.frameRate = frameRate;
        clip.duration = std::max(
            static_cast<float>(baked->playback_time_end - baked->playback_time_begin), 1e-6f);
        clip.looping = true;

        const double timeBegin = baked->playback_time_begin;
        for (std::size_t i = 0; i < baked->nodes.count; ++i) {
            const ufbx_baked_node& node = baked->nodes.data[i];
            if (node.typed_id >= scene->nodes.count) {
                continue;
            }
            const ufbx_node* source = scene->nodes.data[node.typed_id];
            // Hanya bone. Node lain — mesh, kamera, node bantu rigger — tidak
            // punya padanan di rangka, jadi mengimpornya hanya mengisi daftar
            // "track yang tidak menemukan bone-nya" dengan hal yang memang tidak
            // pernah dimaksudkan ke sana.
            if (source == nullptr || source->bone == nullptr || source->name.data == nullptr) {
                continue;
            }
            const std::string bone = source->name.data;

            AddChannel(clip, bone, Channel::TranslationX, node.translation_keys, 0,
                       node.constant_translation, timeBegin);
            AddChannel(clip, bone, Channel::TranslationY, node.translation_keys, 1,
                       node.constant_translation, timeBegin);
            AddChannel(clip, bone, Channel::TranslationZ, node.translation_keys, 2,
                       node.constant_translation, timeBegin);
            AddChannel(clip, bone, Channel::ScaleX, node.scale_keys, 0, node.constant_scale,
                       timeBegin);
            AddChannel(clip, bone, Channel::ScaleY, node.scale_keys, 1, node.constant_scale,
                       timeBegin);
            AddChannel(clip, bone, Channel::ScaleZ, node.scale_keys, 2, node.constant_scale,
                       timeBegin);

            // **Rotasi tetap TETAP diimpor, tidak seperti translasi.** Rotasi
            // bind adalah orientasi tulang, bukan panjangnya: sebuah klip yang
            // meluruskan lengan sepanjang durasinya benar-benar menyatakan
            // sesuatu, dan membuangnya akan mengembalikan lengan itu ke pose rig
            // tujuan di tengah animasi yang seharusnya menahannya.
            if (node.rotation_keys.count > 0) {
                const int track = clip.EnsureRotationTrack(bone);
                const bool constant =
                    node.constant_rotation || RotationIsConstant(node.rotation_keys);
                const std::size_t count = constant ? 1u : node.rotation_keys.count;
                for (std::size_t k = 0; k < count; ++k) {
                    const ufbx_baked_quat& key = node.rotation_keys.data[k];
                    clip.RotationTrackAt(track).AddKey(
                        RotationKey{static_cast<float>(key.time - timeBegin),
                                    glm::normalize(ToQuat(key.value))});
                }
            }
        }

        ufbx_free_baked_anim(baked);

        // Take yang tidak menganimasikan satu bone pun dibuang, tidak
        // dikembalikan sebagai klip kosong. Berkas Mixamo selalu membawa sebuah
        // "Take 001" yang persis begitu, dan klip kosong yang ikut terbawa akan
        // muncul di daftar aset sebagai klip yang bisa dipilih dan tidak
        // melakukan apa-apa.
        if (clip.TrackCount() == 0 && clip.RotationTrackCount() == 0) {
            continue;
        }
        clips.push_back(std::move(clip));
    }
    ufbx_free_scene(scene);

    if (clips.size() == 1) {
        clips.front().name = path.stem().string();
    }
    if (clips.empty()) {
        error = "no animation take in this file animates a bone";
    }
    return clips;
}

}  // namespace sim::animation
