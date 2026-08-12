#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Animation/AnimationGraph.h"
#include "Sim/Animation/AnimationIo.h"
#include "Sim/Animation/GraphInstance.h"
#include "Sim/Animation/Clip.h"
#include "Sim/Animation/ClipHistory.h"
#include "Sim/Animation/ClipImport.h"
#include "Sim/Animation/Pose.h"
#include "Sim/Animation/PoseTask.h"
#include "Sim/Animation/Skeleton.h"
#include "Sim/Assets/MeshData.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace sim;
using namespace sim::animation;

namespace {

class TempDir {
public:
    explicit TempDir(const std::string& name)
        : path_(std::filesystem::temp_directory_path() / ("sim-anim-" + name)) {
        std::error_code code;
        std::filesystem::remove_all(path_, code);
        std::filesystem::create_directories(path_, code);
    }
    ~TempDir() {
        std::error_code code;
        std::filesystem::remove_all(path_, code);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::filesystem::path operator/(const std::string& leaf) const { return path_ / leaf; }

private:
    std::filesystem::path path_;
};

Bone MakeBone(const std::string& name, int parent, const Vec3& offset) {
    Bone bone;
    bone.name = name;
    bone.parent = parent;
    bone.bind.translation = offset;
    return bone;
}

/// Rantai lurus sepanjang Y: Root → Bone1 → Bone2 → ...
Skeleton MakeChain(int boneCount, float segment = 1.0f) {
    Skeleton skeleton;
    for (int i = 0; i < boneCount; ++i) {
        skeleton.AddBone(MakeBone(i == 0 ? "Root" : "Bone" + std::to_string(i), i - 1,
                                  i == 0 ? Vec3(0.0f) : Vec3(0.0f, segment, 0.0f)));
    }
    return skeleton;
}

Curve ConstantCurve(float value) {
    Curve curve;
    CurveKey key;
    key.time = 0.0f;
    key.value = value;
    key.interpolation = Interpolation::Linear;
    curve.AddKey(key);
    return curve;
}

Curve RampCurve(float from, float to, float duration) {
    Curve curve;
    CurveKey a;
    a.time = 0.0f;
    a.value = from;
    a.interpolation = Interpolation::Linear;
    curve.AddKey(a);
    CurveKey b;
    b.time = duration;
    b.value = to;
    b.interpolation = Interpolation::Linear;
    curve.AddKey(b);
    return curve;
}

bool NearlyEqual(const Quat& a, const Quat& b, float epsilon = 0.001f) {
    // Dua kuaternion bertanda berlawanan mewakili rotasi yang sama, jadi yang
    // dibandingkan besarnya hasil kali titik, bukan komponennya.
    return std::abs(std::abs(glm::dot(a, b)) - 1.0f) < epsilon;
}

}  // namespace

// --- rangka -------------------------------------------------------------------

TEST_CASE("Bone selalu tersimpan sesudah induknya") {
    Skeleton skeleton;
    REQUIRE(skeleton.AddBone(MakeBone("Root", -1, Vec3(0.0f))) == 0);
    REQUIRE(skeleton.AddBone(MakeBone("Hips", 0, Vec3(0.0f, 1.0f, 0.0f))) == 1);
    // Induk yang belum ada ditolak — kalau tidak, urutan topologisnya batal dan
    // seluruh penghitungan pose ikut batal bersamanya.
    CHECK(skeleton.AddBone(MakeBone("Bad", 5, Vec3(0.0f))) == -1);
    CHECK(skeleton.AddBone(MakeBone("Root", 0, Vec3(0.0f))) == -1);  // nama kembar
    CHECK(skeleton.AddBone(MakeBone("", 0, Vec3(0.0f))) == -1);      // nama kosong
    CHECK(skeleton.BoneCount() == 2);
}

TEST_CASE("Daftar bone yang melanggar urutan topologis ditolak seluruhnya") {
    Skeleton skeleton;
    skeleton.AddBone(MakeBone("Keep", -1, Vec3(0.0f)));

    std::vector<Bone> bad{MakeBone("A", 1, Vec3(0.0f)), MakeBone("B", -1, Vec3(0.0f))};
    CHECK(!skeleton.SetBones(bad));
    // Yang lama tetap utuh: penolakan tidak boleh setengah jalan.
    CHECK(skeleton.BoneCount() == 1);
    CHECK(skeleton.Bone(0).name == "Keep");
}

TEST_CASE("Menghapus bone ikut membawa keturunannya dan menyusun ulang indeks") {
    Skeleton skeleton;
    skeleton.AddBone(MakeBone("Root", -1, Vec3(0.0f)));
    skeleton.AddBone(MakeBone("Spine", 0, Vec3(0.0f, 1.0f, 0.0f)));
    skeleton.AddBone(MakeBone("ArmL", 1, Vec3(-1.0f, 0.0f, 0.0f)));
    skeleton.AddBone(MakeBone("HandL", 2, Vec3(-1.0f, 0.0f, 0.0f)));
    skeleton.AddBone(MakeBone("ArmR", 1, Vec3(1.0f, 0.0f, 0.0f)));
    REQUIRE(skeleton.BoneCount() == 5);

    REQUIRE(skeleton.RemoveBone(skeleton.Find("ArmL")));
    CHECK(skeleton.BoneCount() == 3);
    CHECK(skeleton.Find("HandL") == -1);  // anaknya ikut terhapus
    const int armR = skeleton.Find("ArmR");
    REQUIRE(armR >= 0);
    // Indeks induk ArmR bergeser dari 1 ke 1 — tapi yang penting ia masih
    // menunjuk Spine, bukan menunjuk indeks yang isinya sudah berganti.
    CHECK(skeleton.Bone(armR).parent == skeleton.Find("Spine"));
    for (int i = 0; i < skeleton.BoneCount(); ++i) {
        REQUIRE(skeleton.Bone(i).parent < i);
    }
}

TEST_CASE("Bind pose global menumpuk sepanjang rantai") {
    const Skeleton skeleton = MakeChain(4, 2.0f);
    const std::vector<BoneTransform>& global = skeleton.GlobalBind();
    REQUIRE(global.size() == 4);
    CHECK(global[0].translation.y == doctest::Approx(0.0f));
    CHECK(global[3].translation.y == doctest::Approx(6.0f));
}

TEST_CASE("Matriks invers bind membatalkan bind pose") {
    const Skeleton skeleton = MakeChain(4, 2.0f);
    const std::vector<Mat4>& inverse = skeleton.InverseBindMatrices();
    const std::vector<BoneTransform>& global = skeleton.GlobalBind();
    for (std::size_t i = 0; i < inverse.size(); ++i) {
        const Mat4 identity = global[i].ToMatrix() * inverse[i];
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                REQUIRE(identity[row][column] ==
                        doctest::Approx(row == column ? 1.0f : 0.0f).epsilon(0.001));
            }
        }
    }
}

TEST_CASE("Pose menggerakkan kulit persis sejauh bone yang mengulitinya") {
    // **Rantai penuhnya, dari pose sampai vertex.** `Pose::ComputeSkinning`
    // menghasilkan palet, `assets::SkinPoint` menerapkannya — dan yang terakhir
    // adalah acuan CPU untuk `skinMatrix` di `Shaders/skin_common.slang`. Yang
    // diuji di sini karena itu bukan salah satunya melainkan bahwa keduanya
    // bersambung: palet yang benar dengan penerapan yang salah, atau sebaliknya,
    // sama-sama menghasilkan karakter yang cacat tanpa satu pun galat.
    const Skeleton skeleton = MakeChain(2);  // Root di 0, Bone1 di y = 1.
    Pose pose(skeleton);
    std::vector<Mat4> palette;

    // Bind pose: paletnya satuan, jadi vertex tidak boleh bergeser sedikit pun.
    pose.ComputeSkinning(skeleton, palette);
    REQUIRE(palette.size() == 2);

    sim::assets::SkinInfluence toChild;
    toChild.bones = {1, 0, 0, 0};
    toChild.weights = {1.0f, 0.0f, 0.0f, 0.0f};
    const Vec3 tip(0.0f, 2.0f, 0.0f);  // Satu meter di atas Bone1.
    Vec3 skinned = sim::assets::SkinPoint(toChild, palette, tip);
    CHECK(skinned.x == doctest::Approx(0.0f));
    CHECK(skinned.y == doctest::Approx(2.0f));

    // Bone1 diputar 90° pada sumbu Z. Ia berpusat di y = 1, jadi titik satu meter
    // di atasnya harus mendarat satu meter di sebelah kirinya: (-1, 1, 0).
    pose.Local(1).rotation = glm::angleAxis(glm::radians(90.0f), Vec3(0.0f, 0.0f, 1.0f));
    pose.ComputeSkinning(skeleton, palette);
    skinned = sim::assets::SkinPoint(toChild, palette, tip);
    CHECK(skinned.x == doctest::Approx(-1.0f).epsilon(1e-4f));
    CHECK(skinned.y == doctest::Approx(1.0f).epsilon(1e-4f));
    CHECK(skinned.z == doctest::Approx(0.0f).epsilon(1e-4f));

    // Vertex yang seluruhnya mengikuti Root tidak ikut bergerak — kalau ia ikut,
    // yang salah adalah paletnya, bukan penerapannya.
    sim::assets::SkinInfluence toRoot;
    toRoot.bones = {0, 0, 0, 0};
    toRoot.weights = {1.0f, 0.0f, 0.0f, 0.0f};
    CHECK(sim::assets::SkinPoint(toRoot, palette, tip).y == doctest::Approx(2.0f));

    // Setengah-setengah mendarat di antara keduanya. Angkanya bukan titik tengah
    // busur — linear blend skinning memang menyusutkan sendi yang ditekuk, dan
    // itu sifat metodenya, bukan cacat penerapannya.
    sim::assets::SkinInfluence half;
    half.bones = {0, 1, 0, 0};
    half.weights = {0.5f, 0.5f, 0.0f, 0.0f};
    const Vec3 middle = sim::assets::SkinPoint(half, palette, tip);
    CHECK(middle.x == doctest::Approx(-0.5f).epsilon(1e-4f));
    CHECK(middle.y == doctest::Approx(1.5f).epsilon(1e-4f));
}

TEST_CASE("Euler bolak-balik lewat kuaternion") {
    const std::vector<Vec3> angles{
        Vec3(0.0f), Vec3(0.3f, -0.7f, 1.2f), Vec3(-1.1f, 0.2f, -0.4f), Vec3(kHalfPi * 0.5f, 0.0f, 0.0f),
    };
    for (const Vec3& euler : angles) {
        const Quat rotation = EulerToQuat(euler);
        const Vec3 back = QuatToEuler(rotation);
        // Yang harus sama adalah rotasinya, bukan angkanya: sudut yang berbeda
        // bisa mewakili rotasi yang sama persis.
        CHECK(NearlyEqual(rotation, EulerToQuat(back)));
    }
}

// --- pose dan pencampuran -----------------------------------------------------

TEST_CASE("Pose baru sama dengan bind pose rangkanya") {
    const Skeleton skeleton = MakeChain(3);
    const Pose pose(skeleton);
    REQUIRE(pose.BoneCount() == 3);
    for (int i = 0; i < 3; ++i) {
        CHECK(pose.Local(i).translation == skeleton.Bone(i).bind.translation);
    }
}

TEST_CASE("Mencampur dua pose pada bobot ujung mengembalikan salah satunya utuh") {
    const Skeleton skeleton = MakeChain(3);
    Pose a(skeleton);
    Pose b(skeleton);
    b.Local(1).rotation = EulerToQuat(Vec3(0.0f, 0.0f, 1.0f));
    b.Local(1).translation = Vec3(5.0f, 0.0f, 0.0f);

    Pose out;
    Blend(a, b, 0.0f, out);
    // Byte demi byte, bukan sekadar mendekati: bobot nol yang menggeser bit
    // terakhir akan menumpuk pada layer yang dicampur tiap frame.
    CHECK(out.Local(1).translation == a.Local(1).translation);
    CHECK(out.Local(1).rotation == a.Local(1).rotation);

    Blend(a, b, 1.0f, out);
    CHECK(out.Local(1).translation == b.Local(1).translation);
    CHECK(out.Local(1).rotation == b.Local(1).rotation);
}

TEST_CASE("Rotasi dicampur lewat jalan terpendek") {
    const Skeleton skeleton = MakeChain(2);
    Pose a(skeleton);
    Pose b(skeleton);
    a.Local(1).rotation = EulerToQuat(Vec3(0.0f, 0.0f, 0.1f));
    // Kuaternion yang sama tapi bertanda terbalik: mewakili rotasi yang sama,
    // dan lerp komponen apa adanya akan memutarinya hampir 360 derajat.
    const Quat target = EulerToQuat(Vec3(0.0f, 0.0f, 0.3f));
    b.Local(1).rotation = Quat(-target.w, -target.x, -target.y, -target.z);

    Pose out;
    Blend(a, b, 0.5f, out);
    const Quat expected = EulerToQuat(Vec3(0.0f, 0.0f, 0.2f));
    CHECK(NearlyEqual(out.Local(1).rotation, expected, 0.01f));
}

TEST_CASE("Bone mask membatasi pencampuran ke rantai yang dipilih") {
    Skeleton skeleton;
    skeleton.AddBone(MakeBone("Root", -1, Vec3(0.0f)));
    skeleton.AddBone(MakeBone("Spine", 0, Vec3(0.0f, 1.0f, 0.0f)));
    skeleton.AddBone(MakeBone("Arm", 1, Vec3(1.0f, 0.0f, 0.0f)));
    skeleton.AddBone(MakeBone("Leg", 0, Vec3(0.0f, -1.0f, 0.0f)));

    Pose a(skeleton);
    Pose b(skeleton);
    for (int i = 0; i < skeleton.BoneCount(); ++i) {
        b.Local(i).translation = Vec3(10.0f, 10.0f, 10.0f);
    }

    BoneMask mask;
    mask.Reset(skeleton.BoneCount(), 0.0f);
    mask.SetChainWeight(skeleton, skeleton.Find("Spine"), 1.0f);

    Pose out;
    Blend(a, b, 1.0f, mask, out);
    CHECK(out.Local(skeleton.Find("Spine")).translation == b.Local(1).translation);
    CHECK(out.Local(skeleton.Find("Arm")).translation == b.Local(2).translation);
    // Kaki di luar mask — ia tidak boleh ikut bergerak sama sekali.
    CHECK(out.Local(skeleton.Find("Leg")).translation == a.Local(3).translation);
    CHECK(out.Local(skeleton.Find("Root")).translation == a.Local(0).translation);
}

TEST_CASE("Layer aditif menambah selisih terhadap pose acuan, bukan terhadap bind") {
    const Skeleton skeleton = MakeChain(2);
    Pose base(skeleton);
    base.Local(1).translation = Vec3(0.0f, 5.0f, 0.0f);

    Pose reference(skeleton);
    reference.Local(1).translation = Vec3(0.0f, 1.0f, 0.0f);
    Pose additive(skeleton);
    additive.Local(1).translation = Vec3(0.0f, 3.0f, 0.0f);  // +2 terhadap acuan

    BoneMask none;
    Pose out;
    BlendAdditive(base, additive, reference, 1.0f, none, out);
    CHECK(out.Local(1).translation.y == doctest::Approx(7.0f));
    BlendAdditive(base, additive, reference, 0.5f, none, out);
    CHECK(out.Local(1).translation.y == doctest::Approx(6.0f));
}

// --- klip dan sampling --------------------------------------------------------

TEST_CASE("Bone tanpa track tetap berdiri di bind pose") {
    const Skeleton skeleton = MakeChain(3, 2.0f);
    Clip clip;
    clip.duration = 1.0f;
    const int track = clip.EnsureTrack("Bone1", Channel::TranslationX);
    clip.TrackAt(track).curve = ConstantCurve(7.0f);

    ClipBinding binding;
    binding.Bind(clip, skeleton);
    Pose pose;
    SampleClip(clip, binding, skeleton, 0.5f, pose);

    CHECK(pose.Local(1).translation.x == doctest::Approx(7.0f));
    // Y tidak punya track — ia harus tetap 2.0 dari bind, bukan nol.
    CHECK(pose.Local(1).translation.y == doctest::Approx(2.0f));
    CHECK(pose.Local(2).translation.y == doctest::Approx(2.0f));
    CHECK(pose.Local(1).scale.x == doctest::Approx(1.0f));
}

TEST_CASE("Kanal rotasi yang tidak punya track diambil dari bind pose") {
    Skeleton skeleton;
    Bone bone = MakeBone("Root", -1, Vec3(0.0f));
    bone.bind.rotation = EulerToQuat(Vec3(0.4f, 0.0f, 0.0f));
    skeleton.AddBone(bone);

    Clip clip;
    const int track = clip.EnsureTrack("Root", Channel::RotationY);
    clip.TrackAt(track).curve = ConstantCurve(1.0f);

    ClipBinding binding;
    binding.Bind(clip, skeleton);
    Pose pose;
    SampleClip(clip, binding, skeleton, 0.0f, pose);

    // Pitch dari bind pose bertahan; yaw datang dari klip.
    const Vec3 euler = QuatToEuler(pose.Local(0).rotation);
    CHECK(euler.x == doctest::Approx(0.4f).epsilon(0.01));
    CHECK(euler.y == doctest::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("Track menunjuk bone lewat nama, jadi menyisipkan bone tidak menggesernya") {
    Skeleton skeleton = MakeChain(3);
    Clip clip;
    const int track = clip.EnsureTrack("Bone2", Channel::TranslationX);
    clip.TrackAt(track).curve = ConstantCurve(9.0f);

    ClipBinding before;
    before.Bind(clip, skeleton);
    Pose poseBefore;
    SampleClip(clip, before, skeleton, 0.0f, poseBefore);
    REQUIRE(poseBefore.Local(skeleton.Find("Bone2")).translation.x == doctest::Approx(9.0f));

    // Satu bone disisipkan; seluruh indeks sesudahnya bergeser.
    Skeleton grown;
    grown.AddBone(MakeBone("Root", -1, Vec3(0.0f)));
    grown.AddBone(MakeBone("Extra", 0, Vec3(0.0f)));
    grown.AddBone(MakeBone("Bone1", 0, Vec3(0.0f, 1.0f, 0.0f)));
    grown.AddBone(MakeBone("Bone2", 2, Vec3(0.0f, 1.0f, 0.0f)));

    ClipBinding after;
    after.Bind(clip, grown);
    Pose poseAfter;
    SampleClip(clip, after, grown, 0.0f, poseAfter);
    CHECK(poseAfter.Local(grown.Find("Bone2")).translation.x == doctest::Approx(9.0f));
    CHECK(after.Unresolved().empty());
}

TEST_CASE("Track yang tidak menemukan bone-nya dilaporkan, bukan didiamkan") {
    const Skeleton skeleton = MakeChain(2);
    Clip clip;
    clip.EnsureTrack("TidakAda", Channel::TranslationX);
    ClipBinding binding;
    binding.Bind(clip, skeleton);
    REQUIRE(binding.Unresolved().size() == 1);
    CHECK(binding.Unresolved()[0] == "TidakAda");
}

// --- retarget -----------------------------------------------------------------

TEST_CASE("Kamus retarget memasang klip ke rig bernama lain") {
    Skeleton target;
    target.AddBone(MakeBone("root_JNT", -1, Vec3(0.0f)));
    target.AddBone(MakeBone("spine_JNT", 0, Vec3(0.0f, 1.0f, 0.0f)));

    Clip clip;
    const int track = clip.EnsureTrack("Spine", Channel::TranslationZ);
    clip.TrackAt(track).curve = ConstantCurve(3.0f);

    RetargetMap map;
    map.Set("Spine", "spine_JNT");

    ClipBinding binding;
    binding.Bind(clip, target, map);
    CHECK(binding.Unresolved().empty());

    Pose pose;
    SampleClip(clip, binding, target, 0.0f, pose);
    CHECK(pose.Local(target.Find("spine_JNT")).translation.z == doctest::Approx(3.0f));
}

TEST_CASE("Kamus dua rig disusun lewat nama standar") {
    RetargetMap sourceToStandard;
    sourceToStandard.Set("Bip01_Spine", "Spine");
    sourceToStandard.Set("Bip01_Head", "Head");
    sourceToStandard.Set("Bip01_Tail", "Tail");

    RetargetMap targetToStandard;
    targetToStandard.Set("spine_JNT", "Spine");
    targetToStandard.Set("head_JNT", "Head");

    const RetargetMap composed = ComposeRetarget(sourceToStandard, targetToStandard);
    CHECK(composed.Resolve("Bip01_Spine") == "spine_JNT");
    CHECK(composed.Resolve("Bip01_Head") == "head_JNT");
    // Rig tujuan tidak punya ekor. Namanya tidak diteruskan apa adanya — kalau
    // diteruskan, ia bisa kebetulan cocok dengan bone yang tidak ada
    // hubungannya.
    CHECK(composed.Resolve("Bip01_Tail") == "Bip01_Tail");
    CHECK(composed.Entries().size() == 2);
}

// --- event --------------------------------------------------------------------

TEST_CASE("Event menyala tepat sekali per lintasan") {
    Clip clip;
    clip.duration = 1.0f;
    clip.AddEvent(Event{0.5f, "footstep"});

    std::vector<const Event*> fired;
    clip.CollectEvents(0.0f, 0.4f, fired);
    CHECK(fired.empty());
    clip.CollectEvents(0.4f, 0.6f, fired);
    CHECK(fired.size() == 1);
    // Frame berikutnya tidak boleh menyalakannya lagi — dengan selang tertutup
    // ia akan menyala dua kali tepat di batas frame.
    clip.CollectEvents(0.6f, 0.8f, fired);
    CHECK(fired.empty());
}

TEST_CASE("Event pada waktu nol menyala di frame pertama") {
    Clip clip;
    clip.duration = 1.0f;
    clip.AddEvent(Event{0.0f, "start"});
    std::vector<const Event*> fired;
    clip.CollectEvents(0.0f, 0.016f, fired);
    REQUIRE(fired.size() == 1);
    CHECK(fired[0]->name == "start");
}

TEST_CASE("Lintasan yang membungkus menyalakan ekor lalu kepala") {
    Clip clip;
    clip.duration = 1.0f;
    clip.AddEvent(Event{0.1f, "awal"});
    clip.AddEvent(Event{0.9f, "akhir"});

    std::vector<const Event*> fired;
    clip.CollectEvents(0.85f, 0.15f, fired);
    REQUIRE(fired.size() == 2);
    // Urutannya berarti: yang di detik terakhir memang terjadi lebih dulu.
    CHECK(fired[0]->name == "akhir");
    CHECK(fired[1]->name == "awal");
}

TEST_CASE("Frame yang panjang tidak melewatkan event") {
    Clip clip;
    clip.duration = 1.0f;
    for (int i = 1; i <= 5; ++i) {
        clip.AddEvent(Event{static_cast<float>(i) * 0.1f, "e" + std::to_string(i)});
    }
    std::vector<const Event*> fired;
    // Satu frame yang tersendat melompati lima event sekaligus; semuanya harus
    // tetap menyala, karena event yang hilang di mesin lambat adalah bug yang
    // hanya muncul di mesin lambat.
    clip.CollectEvents(0.05f, 0.95f, fired);
    CHECK(fired.size() == 5);
}

// --- penanda fase -------------------------------------------------------------

TEST_CASE("Penanda fase memadankan langkah dua klip berdurasi berbeda") {
    Clip walk;
    walk.duration = 1.0f;
    walk.SetSyncMarkers({SyncMarker{0.0f, "LeftFoot"}, SyncMarker{0.5f, "RightFoot"}});

    Clip run;
    run.duration = 0.6f;
    run.SetSyncMarkers({SyncMarker{0.0f, "LeftFoot"}, SyncMarker{0.3f, "RightFoot"}});

    // Tepat di penanda, hasilnya tepat di penanda pasangannya.
    CHECK(walk.MatchPhase(0.0f, run) == doctest::Approx(0.0f));
    CHECK(walk.MatchPhase(0.5f, run) == doctest::Approx(0.3f));
    // Setengah jalan antara dua penanda, hasilnya setengah jalan juga.
    CHECK(walk.MatchPhase(0.25f, run) == doctest::Approx(0.15f));
    CHECK(walk.MatchPhase(0.75f, run) == doctest::Approx(0.45f));
}

TEST_CASE("Tanpa penanda, pemadanan fase jatuh ke waktu ternormalisasi") {
    Clip walk;
    walk.duration = 1.0f;
    Clip run;
    run.duration = 0.5f;
    CHECK(walk.MatchPhase(0.5f, run) == doctest::Approx(0.25f));

    // Jumlah penanda yang berbeda juga jatuh ke sana — bukan memaksakan
    // pasangan yang tidak ada.
    walk.SetSyncMarkers({SyncMarker{0.0f, "A"}});
    run.SetSyncMarkers({SyncMarker{0.0f, "A"}, SyncMarker{0.25f, "B"}});
    CHECK(walk.MatchPhase(0.5f, run) == doctest::Approx(0.25f));
}

// --- root motion --------------------------------------------------------------

TEST_CASE("Root motion diangkat keluar dari pose") {
    Skeleton skeleton = MakeChain(2);
    Clip clip;
    clip.duration = 1.0f;
    clip.extractRootMotion = true;
    clip.rootBone = "Root";
    const int track = clip.EnsureTrack("Root", Channel::TranslationZ);
    clip.TrackAt(track).curve = RampCurve(0.0f, 4.0f, 1.0f);

    ClipBinding binding;
    binding.Bind(clip, skeleton);
    Pose pose;
    SampleClip(clip, binding, skeleton, 0.5f, pose);
    // Root tetap di titik nol: gerakannya sudah diserahkan lewat SampleRootMotion.
    // Kalau ia dibiarkan di pose juga, karakter bergerak dua kali lebih cepat.
    CHECK(pose.Local(0).translation.z == doctest::Approx(0.0f));

    const BoneTransform delta = SampleRootMotion(clip, binding, skeleton, 0.25f, 0.75f);
    CHECK(delta.translation.z == doctest::Approx(2.0f));
}

TEST_CASE("Root motion yang membungkus tidak melempar karakter mundur") {
    Skeleton skeleton = MakeChain(2);
    Clip clip;
    clip.duration = 1.0f;
    clip.looping = true;
    clip.extractRootMotion = true;
    const int track = clip.EnsureTrack("Root", Channel::TranslationZ);
    clip.TrackAt(track).curve = RampCurve(0.0f, 4.0f, 1.0f);

    ClipBinding binding;
    binding.Bind(clip, skeleton);
    // Menyeberangi ujung klip: 0,9 → 0,1. Selisih langsung akan memberi -3,2 —
    // karakter melompat mundur sepanjang seluruh klip tepat saat ia berulang.
    const BoneTransform delta = SampleRootMotion(clip, binding, skeleton, 0.9f, 0.1f);
    CHECK(delta.translation.z == doctest::Approx(0.8f).epsilon(0.001));
}

// --- berkas -------------------------------------------------------------------

TEST_CASE("Rangka bolak-balik lewat berkas") {
    TempDir dir("skel");
    Skeleton skeleton;
    Bone root = MakeBone("Root", -1, Vec3(1.0f, 2.0f, 3.0f));
    root.bind.rotation = EulerToQuat(Vec3(0.3f, -0.4f, 0.5f));
    root.bind.scale = Vec3(2.0f, 2.0f, 2.0f);
    skeleton.AddBone(root);
    skeleton.AddBone(MakeBone("Spine", 0, Vec3(0.0f, 1.0f, 0.0f)));

    SkeletonDocument document;
    document.name = "Biped";
    document.retarget.Set("Spine", "Spine");
    document.retarget.Set("Root", "Hips");

    const std::filesystem::path path = dir / "Biped.simskel";
    REQUIRE(SaveSkeleton(skeleton, document, path).ok);

    Skeleton loaded;
    SkeletonDocument loadedDocument;
    REQUIRE(LoadSkeleton(loaded, loadedDocument, path).ok);
    CHECK(loadedDocument.name == "Biped");
    REQUIRE(loaded.BoneCount() == 2);
    CHECK(loaded.Bone(0).name == "Root");
    CHECK(loaded.Bone(0).bind.translation.x == doctest::Approx(1.0f));
    CHECK(loaded.Bone(0).bind.scale.y == doctest::Approx(2.0f));
    CHECK(NearlyEqual(loaded.Bone(0).bind.rotation, skeleton.Bone(0).bind.rotation));
    CHECK(loaded.Bone(1).parent == 0);
    CHECK(loadedDocument.retarget.Resolve("Root") == "Hips");

    // Menyimpan dokumen yang tidak disunting menghasilkan byte yang sama.
    CHECK(SaveSkeletonToString(loadedDocument, loaded) ==
          SaveSkeletonToString(document, skeleton));
}

TEST_CASE("Rangka yang melanggar urutan topologis ditolak saat dimuat") {
    const std::string text = R"({"version":1,"name":"Bad","bones":[
        {"name":"Child","parent":1},{"name":"Parent","parent":-1}]})";
    Skeleton skeleton;
    SkeletonDocument document;
    const AnimationIoResult result = LoadSkeletonFromString(document, skeleton, text);
    CHECK(!result.ok);
    CHECK(result.error.find("topological") != std::string::npos);
}

TEST_CASE("Klip bolak-balik lewat berkas beserta event dan penanda fase") {
    TempDir dir("clip");
    Clip clip;
    clip.name = "Walk";
    clip.duration = 1.5f;
    clip.frameRate = 24.0f;
    clip.looping = false;
    clip.extractRootMotion = true;
    clip.rootBone = "Root";
    const int track = clip.EnsureTrack("Hips", Channel::RotationY);
    CurveKey key;
    key.time = 0.25f;
    key.value = 0.75f;
    key.inTangent = 1.5f;
    key.outTangent = -0.5f;
    key.interpolation = Interpolation::Bezier;
    clip.TrackAt(track).curve.AddKey(key);
    clip.AddEvent(Event{0.5f, "footstep_left"});
    clip.SetSyncMarkers({SyncMarker{0.0f, "LeftFoot"}, SyncMarker{0.75f, "RightFoot"}});

    ClipDocument document;
    const std::filesystem::path path = dir / "Walk.simanim";
    REQUIRE(SaveClip(clip, document, path).ok);

    Clip loaded;
    ClipDocument loadedDocument;
    REQUIRE(LoadClip(loaded, loadedDocument, path).ok);
    CHECK(loaded.name == "Walk");
    CHECK(loaded.duration == doctest::Approx(1.5f));
    CHECK(loaded.frameRate == doctest::Approx(24.0f));
    CHECK(!loaded.looping);
    CHECK(loaded.extractRootMotion);
    CHECK(loaded.rootBone == "Root");
    REQUIRE(loaded.TrackCount() == 1);
    CHECK(loaded.TrackAt(0).bone == "Hips");
    CHECK(loaded.TrackAt(0).channel == Channel::RotationY);
    REQUIRE(loaded.TrackAt(0).curve.Keys().size() == 1);
    const CurveKey& back = loaded.TrackAt(0).curve.Keys()[0];
    CHECK(back.time == doctest::Approx(0.25f));
    CHECK(back.value == doctest::Approx(0.75f));
    CHECK(back.inTangent == doctest::Approx(1.5f));
    CHECK(back.outTangent == doctest::Approx(-0.5f));
    CHECK(back.interpolation == Interpolation::Bezier);
    REQUIRE(loaded.Events().size() == 1);
    CHECK(loaded.Events()[0].name == "footstep_left");
    REQUIRE(loaded.SyncMarkers().size() == 2);
    CHECK(loaded.SyncMarkers()[1].name == "RightFoot");

    CHECK(SaveClipToString(loadedDocument, loaded) == SaveClipToString(document, clip));
}

TEST_CASE("Track rotasi kuaternion mencuplik lewat jalur pendek dan menahan di ujungnya") {
    RotationTrack track;
    track.bone = "Hips";
    // Sengaja dimasukkan terbalik: `AddKey` yang harus mengurutkannya, bukan
    // pemanggilnya — track yang tidak terurut membuat pencarian binernya
    // mengembalikan ruas yang salah, tanpa satu pun galat.
    track.AddKey(RotationKey{1.0f, glm::angleAxis(glm::radians(90.0f), Vec3(0.0f, 1.0f, 0.0f))});
    track.AddKey(RotationKey{0.0f, Quat(1.0f, 0.0f, 0.0f, 0.0f)});
    REQUIRE(track.keys.size() == 2);
    CHECK(track.keys[0].time == doctest::Approx(0.0f));

    // Di luar rentang kunci nilainya ditahan, bukan diekstrapolasi — sama dengan
    // `Curve::Evaluate`.
    CHECK(NearlyEqual(track.Evaluate(-5.0f), track.keys[0].rotation));
    CHECK(NearlyEqual(track.Evaluate(9.0f), track.keys[1].rotation));

    // Di tengah, setengah jalan menuju 90 derajat. Nlerp bukan slerp, jadi
    // sudutnya tidak persis 45 derajat — yang harus benar adalah sumbunya dan
    // arah putarnya.
    const Vec3 turned = track.Evaluate(0.5f) * Vec3(1.0f, 0.0f, 0.0f);
    CHECK(turned.y == doctest::Approx(0.0f).epsilon(1e-5f));
    CHECK(turned.x > 0.0f);
    CHECK(turned.z < 0.0f);  // memutar ke arah -Z, sama dengan kunci ujungnya

    // **Jalur pendek.** Kunci kedua dinegasikan mewakili rotasi yang sama persis;
    // mencampurnya apa adanya akan memutar lewat jalan 360 derajat, dan yang
    // terlihat adalah tulang yang berputar penuh di antara dua frame.
    RotationTrack flipped;
    flipped.bone = "Hips";
    flipped.AddKey(track.keys[0]);
    const Quat negated = track.keys[1].rotation;
    flipped.AddKey(RotationKey{1.0f, Quat(-negated.w, -negated.x, -negated.y, -negated.z)});
    const Vec3 sameTurn = flipped.Evaluate(0.5f) * Vec3(1.0f, 0.0f, 0.0f);
    CHECK(sameTurn.x == doctest::Approx(turned.x).epsilon(1e-5f));
    CHECK(sameTurn.z == doctest::Approx(turned.z).epsilon(1e-5f));
}

TEST_CASE("Track rotasi kuaternion menang atas kanal Euler pada bone yang sama") {
    const Skeleton skeleton = MakeChain(2);
    Clip clip;
    clip.duration = 1.0f;

    // Kanal Euler menyuruh Bone1 diam; track kuaternion menyuruhnya berputar.
    // Yang berlaku harus yang dari berkas sumbernya — dan tanpa aturan yang
    // jelas, yang berlaku adalah yang kebetulan dijalankan belakangan.
    const int yaw = clip.EnsureTrack("Bone1", Channel::RotationY);
    clip.TrackAt(yaw).curve.AddKey(CurveKey{0.0f, 0.0f, 0.0f, 0.0f, Interpolation::Linear});
    const int rotation = clip.EnsureRotationTrack("Bone1");
    clip.RotationTrackAt(rotation).AddKey(
        RotationKey{0.0f, glm::angleAxis(glm::radians(90.0f), Vec3(0.0f, 0.0f, 1.0f))});

    ClipBinding binding;
    binding.Bind(clip, skeleton);
    CHECK(binding.BoneForRotationTrack(0) == 1);
    CHECK(binding.Unresolved().empty());

    Pose pose;
    SampleClip(clip, binding, skeleton, 0.0f, pose);
    // Bone1 berpusat di y = 1; titik satu meter di atasnya harus mendarat satu
    // meter di sebelah kirinya.
    std::vector<BoneTransform> global;
    pose.ComputeGlobal(skeleton, global);
    const Vec3 tip = Vec3(global[1].ToMatrix() * Vec4(0.0f, 1.0f, 0.0f, 1.0f));
    CHECK(tip.x == doctest::Approx(-1.0f).epsilon(1e-4f));
    CHECK(tip.y == doctest::Approx(1.0f).epsilon(1e-4f));
}

TEST_CASE("Track rotasi yang tidak menemukan bone-nya dilaporkan, bukan didiamkan") {
    const Skeleton skeleton = MakeChain(2);
    Clip clip;
    clip.EnsureRotationTrack("mixamorig:Hips");
    clip.RotationTrackAt(0).AddKey(RotationKey{0.0f, Quat(1.0f, 0.0f, 0.0f, 0.0f)});

    ClipBinding binding;
    binding.Bind(clip, skeleton);
    CHECK(binding.BoneForRotationTrack(0) == -1);
    // Animasi yang setengah berjalan tanpa sebab yang terlihat lebih buruk
    // daripada anggota badan yang jelas tidak bergerak.
    REQUIRE(binding.Unresolved().size() == 1);
    CHECK(binding.Unresolved()[0] == "mixamorig:Hips");
}

TEST_CASE("Track rotasi kuaternion ikut bolak-balik lewat berkas") {
    TempDir dir("rotclip");
    Clip clip;
    clip.name = "Running";
    const int track = clip.EnsureRotationTrack("mixamorig:Hips");
    clip.RotationTrackAt(track).AddKey(
        RotationKey{0.0f, glm::angleAxis(glm::radians(30.0f), glm::normalize(Vec3(1.0f, 2.0f, 3.0f)))});
    clip.RotationTrackAt(track).AddKey(
        RotationKey{0.5f, glm::angleAxis(glm::radians(-70.0f), Vec3(0.0f, 0.0f, 1.0f))});

    ClipDocument document;
    const std::filesystem::path path = dir / "Running.simanim";
    REQUIRE(SaveClip(clip, document, path).ok);

    Clip loaded;
    ClipDocument loadedDocument;
    REQUIRE(LoadClip(loaded, loadedDocument, path).ok);
    REQUIRE(loaded.RotationTrackCount() == 1);
    CHECK(loaded.RotationTrackAt(0).bone == "mixamorig:Hips");
    REQUIRE(loaded.RotationTrackAt(0).keys.size() == 2);
    // **Urutan x,y,z,w yang salah menghasilkan rotasi yang hampir benar**, dan
    // "hampir" adalah jenis kesalahan yang paling lama tidak ketahuan — jadi
    // yang diperiksa nilainya, bukan sekadar jumlah kuncinya.
    for (std::size_t i = 0; i < 2; ++i) {
        CHECK(NearlyEqual(loaded.RotationTrackAt(0).keys[i].rotation,
                          clip.RotationTrackAt(0).keys[i].rotation));
    }
    CHECK(SaveClipToString(loadedDocument, loaded) == SaveClipToString(document, clip));
}

TEST_CASE("Klip FBX yang diimpor menjaga panjang tulang rignya") {
    // Rig dan klipnya tidak ikut di repo, jadi ujinya berjalan hanya bila
    // ditunjuk keduanya:
    //   SIM_RIG_FBX=/path/rig.fbx SIM_CLIP_FBX=/path/Running.fbx ctest
    const char* rigPath = std::getenv("SIM_RIG_FBX");
    const char* clipPath = std::getenv("SIM_CLIP_FBX");
    if (rigPath == nullptr || clipPath == nullptr || !std::filesystem::exists(rigPath) ||
        !std::filesystem::exists(clipPath)) {
        return;
    }

    std::string error;
    const sim::assets::MeshData rig = sim::assets::LoadMesh(rigPath, error);
    REQUIRE(rig.skeleton.IsValid());

    Skeleton skeleton;
    std::vector<Bone> bones;
    bones.reserve(rig.skeleton.bones.size());
    for (const sim::assets::SkeletonBone& source : rig.skeleton.bones) {
        Bone bone;
        bone.name = source.name;
        bone.parent = source.parent;
        bone.bind.translation = source.translation;
        bone.bind.rotation = source.rotation;
        bone.bind.scale = source.scale;
        bones.push_back(std::move(bone));
    }
    REQUIRE(skeleton.SetBones(bones));

    const std::vector<Clip> clips = ImportClipsFromFbx(clipPath, error);
    INFO("import error: " << error);
    REQUIRE(clips.size() == 1);
    const Clip& clip = clips.front();
    // Take "Take 001" milik berkas Mixamo tidak menganimasikan satu bone pun,
    // dan mengambil take pertama begitu saja akan mengimpornya alih-alih yang
    // berisi.
    CHECK(clip.name == std::filesystem::path(clipPath).stem().string());
    CHECK(clip.duration > 0.05f);
    CHECK(clip.RotationTrackCount() > 0);
    CHECK(clip.frameRate > 1.0f);

    // **Bentuk klip impor: rotasi lewat kuaternion, translasi hanya di bone yang
    // benar-benar berpindah.** Terukur pada `Running.fbx`: 52 track rotasi dan
    // tepat tiga track skalar, ketiganya translasi pinggul. Kanal rotasi Euler
    // tidak boleh muncul sama sekali — memaksa kuaternion ke sana menuntut
    // memilih satu dari dua cabang yang sama sahnya pada tiap kunci.
    std::string translated;
    for (const Track& track : clip.Tracks()) {
        INFO("track skalar " << track.bone << " kanal " << ToString(track.channel));
        CHECK(ChannelGroup(track.channel) != ChannelGroup(Channel::RotationX));
        if (translated.empty()) {
            translated = track.bone;
        }
        // Kalau lebih dari satu bone membawa translasi, salah satunya hampir
        // pasti proporsi rig sumber yang lolos — dan itulah yang membuat klip
        // tidak bisa dipasang ke rig lain.
        CHECK(track.bone == translated);
    }

    ClipBinding binding;
    binding.Bind(clip, skeleton);
    // Nama bone rig dan klipnya sama-sama `mixamorig:*`, jadi tidak boleh ada
    // satu pun track yang menggantung.
    for (const std::string& missing : binding.Unresolved()) {
        INFO("track tanpa bone: " << missing);
        CHECK(false);
    }

    // **Panjang tulang adalah invarian yang tidak bergantung pose mana pun**,
    // jadi ia bisa membandingkan klip dengan rig tanpa tahu apa pun tentang
    // gerakan yang dibawanya. Ia juga yang menangkap kesalahan satuan: mode
    // konversi ruang ufbx yang salah menghasilkan tulang seratus kali terlalu
    // panjang, atau rangka yang runtuh — dan keduanya tidak menghasilkan galat.
    const std::vector<BoneTransform>& bind = skeleton.GlobalBind();
    std::vector<BoneTransform> global;
    Pose pose;
    float worst = 0.0f;
    std::string worstBone;
    float worstTime = 0.0f;
    int compared = 0;
    for (int step = 0; step <= 4; ++step) {
        const float time = clip.duration * static_cast<float>(step) / 4.0f;
        SampleClip(clip, binding, skeleton, time, pose);
        pose.ComputeGlobal(skeleton, global);
        for (int i = 0; i < skeleton.BoneCount(); ++i) {
            const int parent = skeleton.Bone(i).parent;
            if (parent < 0) {
                continue;
            }
            const auto self = static_cast<std::size_t>(i);
            const auto up = static_cast<std::size_t>(parent);
            const float bindLength = glm::length(bind[self].translation - bind[up].translation);
            if (bindLength < 1e-4f) {
                continue;
            }
            const float poseLength =
                glm::length(global[self].translation - global[up].translation);
            const float drift = std::abs(poseLength - bindLength) / bindLength;
            // Nama tulangnya ikut dicatat, bukan hanya angkanya: "ada yang
            // meleset 30%" tidak memberi tahu apa pun tentang di mana harus
            // mencari, dan tulang yang meleset hampir selalu menunjuk langsung
            // ke sebabnya.
            if (drift > worst) {
                worst = drift;
                worstBone = skeleton.Bone(i).name;
                worstTime = time;
            }
            ++compared;
        }
    }
    INFO("panjang tulang terburuk menyimpang " << worst * 100.0f << "% pada '" << worstBone
                                               << "' di detik " << worstTime << ", atas "
                                               << compared << " perbandingan");
    CHECK(compared > 100);
    CHECK(worst < 1e-3f);
}

// --- kriteria terima ----------------------------------------------------------

TEST_CASE("Klip 60 detik pada rig 100 bone bisa di-scrub mulus") {
    // Rig 100 bone, seluruh 9 kanal dianimasikan, kunci tiap 1/30 detik selama
    // 60 detik — 900 track dan 162 ribu kunci. Ini lebih padat daripada klip
    // yang ditulis tangan; yang diuji batas atasnya, bukan kasus nyamannya.
    constexpr int kBones = 100;
    constexpr float kDuration = 60.0f;
    constexpr int kKeysPerTrack = 1800;

    Skeleton skeleton = MakeChain(kBones, 0.5f);
    Clip clip;
    clip.duration = kDuration;
    for (int bone = 0; bone < kBones; ++bone) {
        for (int channel = 0; channel < kChannelCount; ++channel) {
            const int track =
                clip.EnsureTrack(skeleton.Bone(bone).name, static_cast<Channel>(channel));
            Curve& curve = clip.TrackAt(track).curve;
            for (int k = 0; k < kKeysPerTrack; ++k) {
                CurveKey key;
                key.time = static_cast<float>(k) * (kDuration / kKeysPerTrack);
                key.value = std::sin(static_cast<float>(k + bone) * 0.05f);
                key.interpolation = Interpolation::Bezier;
                curve.AddKey(key);
            }
        }
    }
    REQUIRE(clip.TrackCount() == kBones * kChannelCount);

    ClipBinding binding;
    binding.Bind(clip, skeleton);
    Pose pose;
    // Satu pencuplikan lebih dulu supaya buffer kerjanya sudah teralokasi —
    // yang diukur biaya per frame saat pemutaran berjalan, bukan biaya frame
    // pertama.
    SampleClip(clip, binding, skeleton, 0.0f, pose);

    constexpr int kFrames = 600;
    const auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < kFrames; ++frame) {
        // Melompat-lompat ke seluruh penjuru klip, bukan maju berurutan: itu
        // yang dilakukan orang saat men-scrub, dan ia mematikan setiap
        // keuntungan lokalitas yang mungkin menyamarkan pencarian yang lambat.
        const float time = std::fmod(static_cast<float>(frame) * 7.31f, kDuration);
        SampleClip(clip, binding, skeleton, time, pose);
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    const double perFrame = elapsed / kFrames;
    INFO("per frame = " << perFrame << " ms over " << kFrames << " frames");
    // Satu frame pada 60 Hz adalah 16,7 ms. Batas 4 ms menyisakan ruang untuk
    // segala hal lain yang terjadi di frame yang sama.
    CHECK(perFrame < 4.0);
}

TEST_CASE("Mencuplik klip tidak mengalokasi setelah frame pertama") {
    // Kriteria "scrub mulus" pada dasarnya kriteria tentang alokasi: satu
    // `std::vector` lokal di jalur sampling akan berdenyut tiap frame dan
    // terlihat sebagai tersendat, bukan sebagai lambat.
    const Skeleton skeleton = MakeChain(50);
    Clip clip;
    clip.duration = 2.0f;
    for (int bone = 0; bone < 50; ++bone) {
        const int track = clip.EnsureTrack(skeleton.Bone(bone).name, Channel::RotationY);
        clip.TrackAt(track).curve = RampCurve(0.0f, 1.0f, 2.0f);
    }
    ClipBinding binding;
    binding.Bind(clip, skeleton);

    Pose pose;
    SampleClip(clip, binding, skeleton, 0.0f, pose);
    const void* localsBefore = pose.Locals().data();
    const void* eulerBefore = pose.EulerScratch(50).data();
    for (int i = 0; i < 100; ++i) {
        SampleClip(clip, binding, skeleton, static_cast<float>(i) * 0.02f, pose);
    }
    // Alamat buffer yang tidak bergerak berarti tidak ada realokasi.
    CHECK(pose.Locals().data() == localsBefore);
    CHECK(pose.EulerScratch(50).data() == eulerBefore);
}

// =============================================================================
// State machine dan blend tree
// =============================================================================

namespace {

/// Pustaka klip sederhana untuk test — peran yang di editor dipegang
/// AssetDatabase.
class TestClipLibrary final : public ClipLibrary {
public:
    Uuid Add(const Clip& clip) {
        const Uuid guid = Uuid::Generate();
        clips_[guid] = clip;
        return guid;
    }
    const Clip* Find(const Uuid& guid) const override {
        const auto at = clips_.find(guid);
        return at == clips_.end() ? nullptr : &at->second;
    }

private:
    std::map<Uuid, Clip> clips_;
};

/// Motion yang memutar satu klip. Ditulis lewat fungsi, bukan inisialisasi
/// agregat: `Motion` punya field yang bawaannya berarti, dan agregat yang
/// menyebut dua field pertama saja membiarkan sisanya tidak terinisialisasi.
Motion ClipMotion(const Uuid& guid) {
    Motion motion;
    motion.kind = MotionKind::Clip;
    motion.clip = AssetRef{guid};
    return motion;
}

/// Klip satu bone yang menggeser X dari `from` ke `to`.
Clip MakeSlideClip(const std::string& name, float from, float to, float duration) {
    Clip clip;
    clip.name = name;
    clip.duration = duration;
    const int track = clip.EnsureTrack("Bone1", Channel::TranslationX);
    clip.TrackAt(track).curve = RampCurve(from, to, duration);
    return clip;
}

}  // namespace

// --- blend tree 1D ------------------------------------------------------------

TEST_CASE("Blend 1D hanya menyalakan dua simpul yang mengapit") {
    const std::vector<float> positions{0.0f, 2.0f, 5.0f};
    std::vector<float> weights;

    Blend1DWeights(positions, 3.0f, weights);
    REQUIRE(weights.size() == 3);
    // Simpul "diam" di 0 tidak ikut menyumbang sama sekali pada kecepatan 3.
    CHECK(weights[0] == doctest::Approx(0.0f));
    CHECK(weights[1] == doctest::Approx(2.0f / 3.0f));
    CHECK(weights[2] == doctest::Approx(1.0f / 3.0f));
}

TEST_CASE("Blend 1D menahan nilainya di luar simpul terluar") {
    const std::vector<float> positions{0.0f, 2.0f, 5.0f};
    std::vector<float> weights;

    Blend1DWeights(positions, -4.0f, weights);
    CHECK(weights[0] == doctest::Approx(1.0f));
    CHECK(weights[2] == doctest::Approx(0.0f));

    Blend1DWeights(positions, 100.0f, weights);
    CHECK(weights[0] == doctest::Approx(0.0f));
    CHECK(weights[2] == doctest::Approx(1.0f));
}

TEST_CASE("Blend 1D tidak menuntut posisi terurut") {
    const std::vector<float> positions{5.0f, 0.0f, 2.0f};
    std::vector<float> weights;
    Blend1DWeights(positions, 3.0f, weights);
    // Urutan keluaran mengikuti urutan masukan, bukan urutan terurut.
    CHECK(weights[0] == doctest::Approx(1.0f / 3.0f));
    CHECK(weights[1] == doctest::Approx(0.0f));
    CHECK(weights[2] == doctest::Approx(2.0f / 3.0f));
}

// --- blend tree 2D ------------------------------------------------------------

TEST_CASE("Blend 2D menghasilkan bobot yang benar pada titik uji") {
    // Empat arah gerak: diam di tengah, maju, mundur, samping.
    const std::vector<Vec2> positions{
        Vec2(0.0f, 0.0f), Vec2(0.0f, 1.0f), Vec2(0.0f, -1.0f), Vec2(1.0f, 0.0f),
    };
    std::vector<float> weights;

    SUBCASE("tepat di sebuah simpul, bobotnya persis satu") {
        for (std::size_t i = 0; i < positions.size(); ++i) {
            Blend2DWeights(positions, positions[i], weights);
            for (std::size_t j = 0; j < weights.size(); ++j) {
                REQUIRE(weights[j] == doctest::Approx(i == j ? 1.0f : 0.0f).epsilon(0.001));
            }
        }
    }

    SUBCASE("setengah jalan antara dua simpul, keduanya berbagi rata") {
        Blend2DWeights(positions, Vec2(0.0f, 0.5f), weights);
        CHECK(weights[0] == doctest::Approx(0.5f).epsilon(0.001));
        CHECK(weights[1] == doctest::Approx(0.5f).epsilon(0.001));
        // Yang berlawanan arah tidak boleh ikut menyumbang.
        CHECK(weights[2] == doctest::Approx(0.0f).epsilon(0.001));
        CHECK(weights[3] == doctest::Approx(0.0f).epsilon(0.001));
    }

    SUBCASE("bobotnya selalu berjumlah satu") {
        const std::vector<Vec2> samples{
            Vec2(0.3f, 0.4f), Vec2(-2.0f, 0.1f), Vec2(0.7f, 0.7f), Vec2(5.0f, -5.0f),
        };
        for (const Vec2& sample : samples) {
            Blend2DWeights(positions, sample, weights);
            float total = 0.0f;
            for (const float weight : weights) {
                REQUIRE(weight >= 0.0f);
                total += weight;
            }
            REQUIRE(total == doctest::Approx(1.0f).epsilon(0.001));
        }
    }

    SUBCASE("di luar sebaran simpul, jatuh ke simpul terluar ke arah itu") {
        Blend2DWeights(positions, Vec2(0.0f, 10.0f), weights);
        CHECK(weights[1] == doctest::Approx(1.0f).epsilon(0.001));
    }
}

TEST_CASE("Blend 2D tidak runtuh saat dua simpul berimpit") {
    const std::vector<Vec2> positions{Vec2(0.0f, 0.0f), Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f)};
    std::vector<float> weights;
    Blend2DWeights(positions, Vec2(0.5f, 0.0f), weights);
    float total = 0.0f;
    for (const float weight : weights) {
        REQUIRE(std::isfinite(weight));
        total += weight;
    }
    CHECK(total == doctest::Approx(1.0f).epsilon(0.001));
}

// --- parameter dan kondisi ----------------------------------------------------

TEST_CASE("Trigger padam sendiri, bool tidak") {
    ParameterSet parameters;
    parameters.Add(Parameter{"Jump", ParameterType::Trigger, 0.0f});
    parameters.Add(Parameter{"Crouching", ParameterType::Bool, 0.0f});

    parameters.Fire("Jump");
    parameters.SetBool("Crouching", true);
    CHECK(parameters.Bool(parameters.Find("Jump")));
    CHECK(parameters.Bool(parameters.Find("Crouching")));

    parameters.ConsumeTriggers();
    // Kejadian padam; keadaan bertahan sampai gameplay yang mematikannya.
    CHECK(!parameters.Bool(parameters.Find("Jump")));
    CHECK(parameters.Bool(parameters.Find("Crouching")));
}

TEST_CASE("Kondisi diuji menurut tipe parameternya") {
    ParameterSet parameters;
    parameters.Add(Parameter{"Speed", ParameterType::Float, 3.0f});
    parameters.Add(Parameter{"Grounded", ParameterType::Bool, 1.0f});
    parameters.Add(Parameter{"Jump", ParameterType::Trigger, 0.0f});

    CHECK(Evaluate(Condition{"Speed", Comparison::Greater, 2.0f}, parameters));
    CHECK(!Evaluate(Condition{"Speed", Comparison::Greater, 5.0f}, parameters));
    CHECK(Evaluate(Condition{"Speed", Comparison::LessEqual, 3.0f}, parameters));

    CHECK(Evaluate(Condition{"Grounded", Comparison::Equal, 1.0f}, parameters));
    CHECK(!Evaluate(Condition{"Grounded", Comparison::Equal, 0.0f}, parameters));

    // Trigger benar semata kalau sedang menyala; pembandingnya diabaikan.
    CHECK(!Evaluate(Condition{"Jump", Comparison::Greater, 0.5f}, parameters));
    parameters.Fire("Jump");
    CHECK(Evaluate(Condition{"Jump", Comparison::Less, -100.0f}, parameters));
}

TEST_CASE("Kondisi yang menunjuk parameter yang tidak ada tidak pernah benar") {
    ParameterSet parameters;
    parameters.Add(Parameter{"Speed", ParameterType::Float, 3.0f});
    // Bukan "selalu benar": transisi yang menyala karena parameternya terhapus
    // adalah kejutan yang paling sulit dilacak.
    CHECK(!Evaluate(Condition{"Hilang", Comparison::Greater, -999.0f}, parameters));
    CHECK(!Evaluate(Condition{"Hilang", Comparison::NotEqual, 12.0f}, parameters));
}

// --- runtime graph ------------------------------------------------------------

namespace {

struct GraphFixture {
    Skeleton skeleton = MakeChain(2);
    TestClipLibrary library;
    AnimationGraph graph;
    GraphInstance instance;

    void Build() { instance.Bind(graph, skeleton, library); }
};

}  // namespace

TEST_CASE("Graph memulai pada state bawaannya") {
    GraphFixture fixture;
    const Uuid idle = fixture.library.Add(MakeSlideClip("Idle", 0.0f, 0.0f, 1.0f));
    const Uuid walk = fixture.library.Add(MakeSlideClip("Walk", 0.0f, 10.0f, 1.0f));

    Layer layer;
    layer.states.push_back(State{"Idle", ClipMotion(idle), Vec2(0.0f)});
    layer.states.push_back(State{"Walk", ClipMotion(walk), Vec2(0.0f)});
    layer.defaultState = 1;
    fixture.graph.AddLayer(layer);
    fixture.Build();

    CHECK(fixture.instance.CurrentState(0) == 1);
    fixture.instance.Update(0.5f);
    // Setengah jalan pada klip Walk: X sudah bergeser separuh.
    CHECK(fixture.instance.Result().Local(1).translation.x == doctest::Approx(5.0f).epsilon(0.01));
}

TEST_CASE("Transisi berkondisi berpindah state dan crossfade-nya mulus") {
    GraphFixture fixture;
    const Uuid idle = fixture.library.Add(MakeSlideClip("Idle", 0.0f, 0.0f, 1.0f));
    const Uuid walk = fixture.library.Add(MakeSlideClip("Walk", 20.0f, 20.0f, 1.0f));

    fixture.graph.parameters.Add(Parameter{"Speed", ParameterType::Float, 0.0f});
    Layer layer;
    layer.states.push_back(State{"Idle", ClipMotion(idle), Vec2(0.0f)});
    layer.states.push_back(State{"Walk", ClipMotion(walk), Vec2(0.0f)});
    Transition go;
    go.from = 0;
    go.to = 1;
    go.duration = 0.4f;
    go.conditions.push_back(Condition{"Speed", Comparison::Greater, 0.5f});
    layer.transitions.push_back(go);
    fixture.graph.AddLayer(layer);
    fixture.Build();

    fixture.instance.Update(0.1f);
    REQUIRE(fixture.instance.CurrentState(0) == 0);

    fixture.instance.parameters.SetFloat("Speed", 3.0f);
    fixture.instance.Update(0.01f);
    REQUIRE(fixture.instance.CurrentState(0) == 1);

    // Di tengah crossfade, hasilnya harus di antara kedua klip — bukan melompat.
    fixture.instance.Update(0.2f);
    const float x = fixture.instance.Result().Local(1).translation.x;
    CHECK(x > 0.5f);
    CHECK(x < 19.5f);
    CHECK(fixture.instance.TransitionProgress(0) > 0.0f);

    // Setelah durasinya lewat, ia sepenuhnya di klip tujuan.
    fixture.instance.Update(0.5f);
    CHECK(fixture.instance.TransitionProgress(0) == doctest::Approx(0.0f));
    CHECK(fixture.instance.Result().Local(1).translation.x == doctest::Approx(20.0f).epsilon(0.01));
}

TEST_CASE("Trigger terlihat seluruh lapis sebelum dipadamkan") {
    GraphFixture fixture;
    const Uuid a = fixture.library.Add(MakeSlideClip("A", 0.0f, 0.0f, 1.0f));
    const Uuid b = fixture.library.Add(MakeSlideClip("B", 1.0f, 1.0f, 1.0f));
    fixture.graph.parameters.Add(Parameter{"Fire", ParameterType::Trigger, 0.0f});

    for (int i = 0; i < 2; ++i) {
        Layer layer;
        layer.name = "L" + std::to_string(i);
        layer.states.push_back(State{"A", ClipMotion(a), Vec2(0.0f)});
        layer.states.push_back(State{"B", ClipMotion(b), Vec2(0.0f)});
        Transition go;
        go.from = 0;
        go.to = 1;
        go.duration = 0.0f;
        go.conditions.push_back(Condition{"Fire", Comparison::Greater, 0.0f});
        layer.transitions.push_back(go);
        fixture.graph.AddLayer(layer);
    }
    fixture.Build();

    fixture.instance.parameters.Fire("Fire");
    fixture.instance.Update(1.0f / 60.0f);
    // Kalau trigger dipadamkan per lapis, lapis kedua tidak akan pernah
    // melihatnya.
    CHECK(fixture.instance.CurrentState(0) == 1);
    CHECK(fixture.instance.CurrentState(1) == 1);
}

TEST_CASE("Transisi dari state mana pun berlaku di semua state") {
    GraphFixture fixture;
    const Uuid a = fixture.library.Add(MakeSlideClip("A", 0.0f, 0.0f, 1.0f));
    const Uuid b = fixture.library.Add(MakeSlideClip("B", 1.0f, 1.0f, 1.0f));
    const Uuid hit = fixture.library.Add(MakeSlideClip("Hit", 9.0f, 9.0f, 1.0f));
    fixture.graph.parameters.Add(Parameter{"Hit", ParameterType::Trigger, 0.0f});

    Layer layer;
    layer.states.push_back(State{"A", ClipMotion(a), Vec2(0.0f)});
    layer.states.push_back(State{"B", ClipMotion(b), Vec2(0.0f)});
    layer.states.push_back(State{"Hit", ClipMotion(hit), Vec2(0.0f)});
    Transition any;
    any.from = -1;  // dari mana pun
    any.to = 2;
    any.duration = 0.0f;
    any.conditions.push_back(Condition{"Hit", Comparison::Greater, 0.0f});
    layer.transitions.push_back(any);
    fixture.graph.AddLayer(layer);
    fixture.Build();

    fixture.instance.Play(0, 1);
    fixture.instance.parameters.Fire("Hit");
    fixture.instance.Update(0.016f);
    CHECK(fixture.instance.CurrentState(0) == 2);
}

TEST_CASE("Exit time menahan transisi sampai klipnya cukup jauh berjalan") {
    GraphFixture fixture;
    const Uuid a = fixture.library.Add(MakeSlideClip("A", 0.0f, 0.0f, 1.0f));
    const Uuid b = fixture.library.Add(MakeSlideClip("B", 1.0f, 1.0f, 1.0f));

    Layer layer;
    layer.states.push_back(State{"A", ClipMotion(a), Vec2(0.0f)});
    layer.states.push_back(State{"B", ClipMotion(b), Vec2(0.0f)});
    Transition go;
    go.from = 0;
    go.to = 1;
    go.duration = 0.0f;
    go.hasExitTime = true;
    go.exitTime = 0.8f;
    layer.transitions.push_back(go);
    fixture.graph.AddLayer(layer);
    fixture.Build();

    fixture.instance.Update(0.5f);
    CHECK(fixture.instance.CurrentState(0) == 0);
    fixture.instance.Update(0.35f);
    CHECK(fixture.instance.CurrentState(0) == 1);
}

TEST_CASE("Blend tree 1D di dalam state mengikuti parameternya") {
    GraphFixture fixture;
    const Uuid idle = fixture.library.Add(MakeSlideClip("Idle", 0.0f, 0.0f, 1.0f));
    const Uuid run = fixture.library.Add(MakeSlideClip("Run", 10.0f, 10.0f, 1.0f));
    fixture.graph.parameters.Add(Parameter{"Speed", ParameterType::Float, 0.0f});

    Motion motion;
    motion.kind = MotionKind::Blend1D;
    motion.parameterX = "Speed";
    motion.syncPhase = false;
    motion.children.push_back(MotionChild{AssetRef{idle}, Vec2(0.0f, 0.0f), 1.0f});
    motion.children.push_back(MotionChild{AssetRef{run}, Vec2(10.0f, 0.0f), 1.0f});

    Layer layer;
    layer.states.push_back(State{"Locomotion", motion, Vec2(0.0f)});
    fixture.graph.AddLayer(layer);
    fixture.Build();

    fixture.instance.parameters.SetFloat("Speed", 5.0f);
    fixture.instance.Update(0.016f);
    CHECK(fixture.instance.Result().Local(1).translation.x == doctest::Approx(5.0f).epsilon(0.05));

    fixture.instance.parameters.SetFloat("Speed", 10.0f);
    fixture.instance.Update(0.016f);
    CHECK(fixture.instance.Result().Local(1).translation.x == doctest::Approx(10.0f).epsilon(0.05));
}

TEST_CASE("Lapis bermask hanya menyentuh rantai bone-nya") {
    Skeleton skeleton;
    skeleton.AddBone(MakeBone("Root", -1, Vec3(0.0f)));
    skeleton.AddBone(MakeBone("Spine", 0, Vec3(0.0f, 1.0f, 0.0f)));
    skeleton.AddBone(MakeBone("Leg", 0, Vec3(0.0f, -1.0f, 0.0f)));

    TestClipLibrary library;
    Clip base;
    base.duration = 1.0f;
    base.TrackAt(base.EnsureTrack("Spine", Channel::TranslationX)).curve = ConstantCurve(1.0f);
    base.TrackAt(base.EnsureTrack("Leg", Channel::TranslationX)).curve = ConstantCurve(1.0f);
    Clip upper;
    upper.duration = 1.0f;
    upper.TrackAt(upper.EnsureTrack("Spine", Channel::TranslationX)).curve = ConstantCurve(9.0f);
    upper.TrackAt(upper.EnsureTrack("Leg", Channel::TranslationX)).curve = ConstantCurve(9.0f);
    const Uuid baseGuid = library.Add(base);
    const Uuid upperGuid = library.Add(upper);

    AnimationGraph graph;
    Layer baseLayer;
    baseLayer.name = "Base";
    baseLayer.states.push_back(
        State{"Base", ClipMotion(baseGuid), Vec2(0.0f)});
    graph.AddLayer(baseLayer);

    Layer upperLayer;
    upperLayer.name = "Upper";
    upperLayer.maskRootBone = "Spine";
    upperLayer.states.push_back(
        State{"Upper", ClipMotion(upperGuid), Vec2(0.0f)});
    graph.AddLayer(upperLayer);

    GraphInstance instance;
    instance.Bind(graph, skeleton, library);
    instance.Update(0.016f);

    CHECK(instance.Result().Local(skeleton.Find("Spine")).translation.x ==
          doctest::Approx(9.0f).epsilon(0.01));
    // Kaki di luar mask lapis atas — ia tetap milik lapis dasar.
    CHECK(instance.Result().Local(skeleton.Find("Leg")).translation.x ==
          doctest::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("Event klip yang sedang diputar dilaporkan runtime") {
    GraphFixture fixture;
    Clip walk = MakeSlideClip("Walk", 0.0f, 1.0f, 1.0f);
    walk.AddEvent(Event{0.5f, "footstep_left"});
    const Uuid guid = fixture.library.Add(walk);

    Layer layer;
    layer.states.push_back(State{"Walk", ClipMotion(guid), Vec2(0.0f)});
    fixture.graph.AddLayer(layer);
    fixture.Build();

    fixture.instance.Update(0.4f);
    CHECK(fixture.instance.Events().empty());
    fixture.instance.Update(0.2f);
    REQUIRE(fixture.instance.Events().size() == 1);
    CHECK(fixture.instance.Events()[0].name == "footstep_left");
    CHECK(fixture.instance.Events()[0].layer == 0);
    // Frame berikutnya tidak menyalakannya lagi.
    fixture.instance.Update(0.2f);
    CHECK(fixture.instance.Events().empty());
}

TEST_CASE("Root motion graph datang dari lapis dasar") {
    GraphFixture fixture;
    Clip walk;
    walk.duration = 1.0f;
    walk.extractRootMotion = true;
    walk.rootBone = "Root";
    walk.TrackAt(walk.EnsureTrack("Root", Channel::TranslationZ)).curve =
        RampCurve(0.0f, 4.0f, 1.0f);
    const Uuid guid = fixture.library.Add(walk);

    Layer layer;
    layer.states.push_back(State{"Walk", ClipMotion(guid), Vec2(0.0f)});
    fixture.graph.AddLayer(layer);
    fixture.Build();

    fixture.instance.Update(0.25f);
    CHECK(fixture.instance.RootMotion().translation.z == doctest::Approx(1.0f).epsilon(0.02));
    // Root tetap di tempat di dalam pose — gerakannya sudah diserahkan.
    CHECK(fixture.instance.Result().Local(0).translation.z == doctest::Approx(0.0f));
}

// --- berkas graph -------------------------------------------------------------

TEST_CASE("Kondisi transisi tersimpan dan dimuat identik") {
    TempDir dir("graph");
    AnimationGraph graph;
    graph.name = "Locomotion";
    graph.parameters.Add(Parameter{"Speed", ParameterType::Float, 1.5f});
    graph.parameters.Add(Parameter{"Grounded", ParameterType::Bool, 1.0f});
    graph.parameters.Add(Parameter{"Jump", ParameterType::Trigger, 0.0f});

    Motion blend;
    blend.kind = MotionKind::Blend2D;
    blend.parameterX = "Speed";
    blend.parameterY = "Grounded";
    blend.speed = 1.25f;
    blend.syncPhase = false;
    blend.children.push_back(MotionChild{AssetRef{Uuid::Generate()}, Vec2(0.0f, 0.0f), 1.0f});
    blend.children.push_back(MotionChild{AssetRef{Uuid::Generate()}, Vec2(3.0f, -1.0f), 0.75f});

    Layer layer;
    layer.name = "Base";
    layer.weight = 0.85f;
    layer.additive = true;
    layer.maskRootBone = "Spine";
    layer.defaultState = 1;
    layer.states.push_back(State{"Idle", Motion{}, Vec2(10.0f, 20.0f)});
    layer.states.push_back(State{"Locomotion", blend, Vec2(-30.0f, 40.0f)});

    Transition transition;
    transition.from = 0;
    transition.to = 1;
    transition.duration = 0.35f;
    transition.hasExitTime = true;
    transition.exitTime = 0.8f;
    // Satu kondisi per tipe parameter, dan pembanding yang berbeda-beda —
    // kriteria terimanya menuntut seluruhnya kembali identik.
    transition.conditions.push_back(Condition{"Speed", Comparison::GreaterEqual, 2.5f});
    transition.conditions.push_back(Condition{"Grounded", Comparison::Equal, 1.0f});
    transition.conditions.push_back(Condition{"Jump", Comparison::NotEqual, -3.25f});
    layer.transitions.push_back(transition);
    graph.AddLayer(layer);

    const std::filesystem::path path = dir / "Locomotion.simanimgraph";
    REQUIRE(SaveGraph(graph, path).ok);

    AnimationGraph loaded;
    REQUIRE(LoadGraph(loaded, path).ok);

    REQUIRE(loaded.LayerCount() == 1);
    const Layer& back = loaded.LayerAt(0);
    CHECK(back.name == "Base");
    CHECK(back.weight == doctest::Approx(0.85f));
    CHECK(back.additive);
    CHECK(back.maskRootBone == "Spine");
    CHECK(back.defaultState == 1);
    REQUIRE(back.transitions.size() == 1);

    const Transition& loadedTransition = back.transitions[0];
    CHECK(loadedTransition.from == 0);
    CHECK(loadedTransition.to == 1);
    CHECK(loadedTransition.duration == doctest::Approx(0.35f));
    CHECK(loadedTransition.hasExitTime);
    CHECK(loadedTransition.exitTime == doctest::Approx(0.8f));
    REQUIRE(loadedTransition.conditions.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        const Condition& want = transition.conditions[i];
        const Condition& got = loadedTransition.conditions[i];
        CHECK(got.parameter == want.parameter);
        CHECK(got.comparison == want.comparison);
        CHECK(got.value == doctest::Approx(want.value));
    }

    REQUIRE(back.states.size() == 2);
    CHECK(back.states[1].motion.kind == MotionKind::Blend2D);
    CHECK(back.states[1].motion.parameterY == "Grounded");
    CHECK(back.states[1].motion.speed == doctest::Approx(1.25f));
    CHECK(!back.states[1].motion.syncPhase);
    REQUIRE(back.states[1].motion.children.size() == 2);
    CHECK(back.states[1].motion.children[1].position.x == doctest::Approx(3.0f));
    CHECK(back.states[1].motion.children[1].position.y == doctest::Approx(-1.0f));
    CHECK(back.states[1].motion.children[1].speed == doctest::Approx(0.75f));
    CHECK(back.states[1].canvas.x == doctest::Approx(-30.0f));

    // Dan byte-nya sama: menyimpan dokumen yang tidak disunting tidak boleh
    // menghasilkan diff.
    CHECK(SaveGraphToString(loaded) == SaveGraphToString(graph));
}

TEST_CASE("Kondisi menunjuk parameter lewat nama, jadi menyisipkan parameter tidak menggesernya") {
    AnimationGraph graph;
    graph.parameters.Add(Parameter{"Speed", ParameterType::Float, 4.0f});
    Layer layer;
    layer.states.push_back(State{"A", Motion{}, Vec2(0.0f)});
    Transition transition;
    transition.conditions.push_back(Condition{"Speed", Comparison::Greater, 1.0f});
    layer.transitions.push_back(transition);
    graph.AddLayer(layer);

    const std::string text = SaveGraphToString(graph);
    AnimationGraph loaded;
    REQUIRE(LoadGraphFromString(loaded, text).ok);

    // Parameter baru disisipkan di depan; kalau kondisinya memakai indeks, ia
    // sekarang menunjuk parameter yang salah.
    std::vector<Parameter> shifted{Parameter{"Baru", ParameterType::Float, 0.0f}};
    for (const Parameter& parameter : loaded.parameters.All()) {
        shifted.push_back(parameter);
    }
    loaded.parameters.SetAll(shifted);
    CHECK(Evaluate(loaded.LayerAt(0).transitions[0].conditions[0], loaded.parameters));
}

// --- riwayat suntingan klip ---------------------------------------------------

TEST_CASE("Memindahkan keyframe bisa dibatalkan") {
    Clip clip;
    clip.duration = 2.0f;
    const int track = clip.EnsureTrack("Bone1", Channel::TranslationX);
    clip.TrackAt(track).curve = RampCurve(0.0f, 10.0f, 2.0f);
    REQUIRE(clip.TrackAt(track).curve.Keys().size() == 2);

    ClipHistory history;
    history.Begin("Move key");
    history.Capture(clip, track);
    clip.TrackAt(track).curve.MoveKey(1, 1.0f, 7.0f);
    history.End();

    REQUIRE(history.UndoDepth() == 1);
    CHECK(history.UndoLabel() == "Move key");
    CHECK(clip.TrackAt(track).curve.Keys()[1].time == doctest::Approx(1.0f));
    CHECK(clip.TrackAt(track).curve.Keys()[1].value == doctest::Approx(7.0f));

    REQUIRE(history.Undo(clip));
    CHECK(clip.TrackAt(track).curve.Keys()[1].time == doctest::Approx(2.0f));
    CHECK(clip.TrackAt(track).curve.Keys()[1].value == doctest::Approx(10.0f));

    REQUIRE(history.Redo(clip));
    CHECK(clip.TrackAt(track).curve.Keys()[1].time == doctest::Approx(1.0f));
    CHECK(clip.TrackAt(track).curve.Keys()[1].value == doctest::Approx(7.0f));
}

TEST_CASE("Kunci yang diseret melewati tetangganya tetap bisa dibatalkan") {
    // Justru kasus inilah alasan riwayatnya menyalin track utuh dan bukan delta
    // per-kunci: `MoveKey` menjaga urutan waktu, jadi kunci yang menyeberang
    // berpindah indeks — dan riwayat yang mencatat indeks akan memulihkan yang
    // salah.
    Clip clip;
    clip.duration = 3.0f;
    const int track = clip.EnsureTrack("Bone1", Channel::TranslationX);
    Curve& curve = clip.TrackAt(track).curve;
    for (int i = 0; i < 3; ++i) {
        CurveKey key;
        key.time = static_cast<float>(i);
        key.value = static_cast<float>(i) * 10.0f;
        key.interpolation = Interpolation::Linear;
        curve.AddKey(key);
    }

    ClipHistory history;
    history.Begin("Drag key");
    history.Capture(clip, track);
    // Kunci pertama diseret melewati dua kunci lain.
    clip.TrackAt(track).curve.MoveKey(0, 2.5f, 0.0f);
    history.End();
    REQUIRE(clip.TrackAt(track).curve.Keys()[2].time == doctest::Approx(2.5f));

    REQUIRE(history.Undo(clip));
    const std::vector<CurveKey>& back = clip.TrackAt(track).curve.Keys();
    REQUIRE(back.size() == 3);
    for (int i = 0; i < 3; ++i) {
        CHECK(back[static_cast<std::size_t>(i)].time == doctest::Approx(static_cast<float>(i)));
        CHECK(back[static_cast<std::size_t>(i)].value ==
              doctest::Approx(static_cast<float>(i) * 10.0f));
    }
}

TEST_CASE("Menambah dan menghapus track bisa dibatalkan") {
    Clip clip;
    const int first = clip.EnsureTrack("Bone1", Channel::TranslationX);
    clip.TrackAt(first).curve = ConstantCurve(1.0f);

    ClipHistory history;
    history.Begin("Add track");
    history.CaptureAllTracks(clip);
    const int added = clip.EnsureTrack("Bone1", Channel::RotationY);
    clip.TrackAt(added).curve = ConstantCurve(2.0f);
    history.End();
    REQUIRE(clip.TrackCount() == 2);

    REQUIRE(history.Undo(clip));
    CHECK(clip.TrackCount() == 1);
    CHECK(clip.TrackAt(0).channel == Channel::TranslationX);
    REQUIRE(history.Redo(clip));
    CHECK(clip.TrackCount() == 2);
}

TEST_CASE("Menyunting event dan penanda fase bisa dibatalkan") {
    Clip clip;
    clip.duration = 1.0f;
    clip.AddEvent(Event{0.5f, "lama"});

    ClipHistory history;
    history.Begin("Add event");
    history.CaptureEvents(clip);
    clip.AddEvent(Event{0.25f, "baru"});
    history.End();
    REQUIRE(clip.Events().size() == 2);

    REQUIRE(history.Undo(clip));
    REQUIRE(clip.Events().size() == 1);
    CHECK(clip.Events()[0].name == "lama");
    REQUIRE(history.Redo(clip));
    CHECK(clip.Events().size() == 2);
}

TEST_CASE("Langkah yang tidak menyalin apa pun tidak masuk riwayat") {
    Clip clip;
    ClipHistory history;
    history.Begin("Tidak jadi");
    history.End();
    CHECK(history.UndoDepth() == 0);
}

TEST_CASE("Menyunting sesudah undo membuang cabang redo") {
    Clip clip;
    const int track = clip.EnsureTrack("Bone1", Channel::TranslationX);
    clip.TrackAt(track).curve = ConstantCurve(1.0f);

    ClipHistory history;
    history.Begin("A");
    history.Capture(clip, track);
    clip.TrackAt(track).curve = ConstantCurve(2.0f);
    history.End();

    REQUIRE(history.Undo(clip));
    REQUIRE(history.RedoDepth() == 1);

    history.Begin("B");
    history.Capture(clip, track);
    clip.TrackAt(track).curve = ConstantCurve(3.0f);
    history.End();
    CHECK(history.RedoDepth() == 0);
    CHECK(history.UndoDepth() == 1);
}


// --- E8.4: sistem task pose (mengikuti Esoterica) -----------------------------

namespace {

/// Task yang mencatat urutan jalannya. Dipakai memeriksa bahwa dependensi
/// benar-benar mendahului yang membutuhkannya.
class RecordingTask final : public PoseTask {
public:
    RecordingTask(std::vector<int>* log, int id, std::vector<TaskIndex> dependencies)
        : PoseTask(std::move(dependencies)), log_(log), id_(id) {}

    int Execute(const TaskContext& context) override {
        log_->push_back(id_);
        if (!dependencies_.empty()) {
            // Memakai ulang buffer dependensi pertama, sama seperti blend.
            return (*context.results)[static_cast<std::size_t>(dependencies_[0])];
        }
        return context.pool->Acquire();
    }
    const char* Name() const override { return "Recording"; }

private:
    std::vector<int>* log_;
    int id_;
};

}  // namespace

TEST_CASE("task dijalankan sesudah dependensinya, dan hanya yang menyumbang") {
    const Skeleton skeleton = MakeChain(3);
    TaskSystem tasks;
    tasks.Reset(skeleton);

    std::vector<int> order;
    const TaskIndex a = tasks.Register(std::make_unique<RecordingTask>(&order, 1, std::vector<TaskIndex>{}));
    const TaskIndex b = tasks.Register(std::make_unique<RecordingTask>(&order, 2, std::vector<TaskIndex>{}));
    const TaskIndex ab = tasks.Register(std::make_unique<RecordingTask>(&order, 3, std::vector<TaskIndex>{a, b}));
    // Task yang tidak menyumbang ke akar. **Ia tidak boleh dijalankan sama
    // sekali:** simpul yang mendaftarkan task lalu dibuang oleh blend berbobot
    // nol tidak boleh membayar pencuplikannya.
    tasks.Register(std::make_unique<RecordingTask>(&order, 99, std::vector<TaskIndex>{}));

    Pose result(skeleton);
    CHECK(tasks.Execute(ab, result, 0.0f));

    REQUIRE(order.size() == 3);
    // Kedua dependensi mendahului yang membutuhkannya.
    const auto positionOf = [&](int id) {
        return std::find(order.begin(), order.end(), id) - order.begin();
    };
    CHECK(positionOf(1) < positionOf(3));
    CHECK(positionOf(2) < positionOf(3));
    CHECK(std::find(order.begin(), order.end(), 99) == order.end());
    CHECK(tasks.ExecutedCount() == 3);
}

TEST_CASE("task yang dipakai dua kali dijalankan sekali") {
    const Skeleton skeleton = MakeChain(3);
    TaskSystem tasks;
    tasks.Reset(skeleton);

    // Inilah yang membuat sistem task berupa DAG dan bukan pohon: satu klip yang
    // dicuplik sekali masuk ke dua blend tanpa dicuplik dua kali. Runtime yang
    // mencampur pose di dalam simpulnya tidak punya tempat untuk menyatakan itu.
    std::vector<int> order;
    const TaskIndex shared = tasks.Register(std::make_unique<RecordingTask>(&order, 1, std::vector<TaskIndex>{}));
    const TaskIndex other = tasks.Register(std::make_unique<RecordingTask>(&order, 2, std::vector<TaskIndex>{}));
    const TaskIndex left = tasks.Register(std::make_unique<RecordingTask>(&order, 3, std::vector<TaskIndex>{shared, other}));
    const TaskIndex right = tasks.Register(std::make_unique<RecordingTask>(&order, 4, std::vector<TaskIndex>{shared, left}));

    Pose result(skeleton);
    CHECK(tasks.Execute(right, result, 0.0f));
    CHECK(std::count(order.begin(), order.end(), 1) == 1);
    CHECK(tasks.ExecutedCount() == 4);
}

TEST_CASE("buffer pose dilepas begitu pemakai terakhirnya selesai") {
    const Skeleton skeleton = MakeChain(4);
    const Clip clipA = MakeSlideClip("A", 0.0f, 10.0f, 1.0f);
    const Clip clipB = MakeSlideClip("B", 0.0f, 20.0f, 1.0f);
    ClipBinding bindA;
    bindA.Bind(clipA, skeleton);
    ClipBinding bindB;
    bindB.Bind(clipB, skeleton);

    TaskSystem tasks;
    tasks.Reset(skeleton);

    // Rantai blend yang dalam. **Pemakaian puncaknya harus tetap**, bukan tumbuh
    // sepanjang rantai: blend menulis ke buffer masukan pertamanya, jadi setiap
    // tingkat melepas satu buffer segera setelah memakainya. Kolam yang tumbuh
    // linear terhadap kedalaman graph adalah kolam yang tidak melepas apa pun.
    TaskIndex current = tasks.Register(std::make_unique<SampleTask>(clipA, bindA, 0.5f));
    for (int i = 0; i < 8; ++i) {
        const TaskIndex next = tasks.Register(std::make_unique<SampleTask>(clipB, bindB, 0.5f));
        current = tasks.Register(std::make_unique<BlendTask>(current, next, 0.5f));
    }

    Pose result(skeleton);
    CHECK(tasks.Execute(current, result, 0.0f));
    CHECK(tasks.Pool().PeakInUse() <= 2);
    // Dan seluruhnya dikembalikan sesudah selesai — angka yang tidak pernah
    // turun berarti ada yang lupa dilepas.
    CHECK(tasks.Pool().InUse() == 0);
}

TEST_CASE("blend task menghasilkan pose yang sama dengan Blend langsung") {
    const Skeleton skeleton = MakeChain(3);
    const Clip clipA = MakeSlideClip("A", 4.0f, 4.0f, 1.0f);
    const Clip clipB = MakeSlideClip("B", 8.0f, 8.0f, 1.0f);
    ClipBinding bindA;
    bindA.Bind(clipA, skeleton);
    ClipBinding bindB;
    bindB.Bind(clipB, skeleton);

    TaskSystem tasks;
    tasks.Reset(skeleton);
    const TaskIndex a = tasks.Register(std::make_unique<SampleTask>(clipA, bindA, 0.0f));
    const TaskIndex b = tasks.Register(std::make_unique<SampleTask>(clipB, bindB, 0.0f));
    const TaskIndex blended = tasks.Register(std::make_unique<BlendTask>(a, b, 0.25f));

    Pose viaTasks(skeleton);
    REQUIRE(tasks.Execute(blended, viaTasks, 0.0f));

    Pose poseA(skeleton);
    Pose poseB(skeleton);
    SampleClip(clipA, bindA, skeleton, 0.0f, poseA);
    SampleClip(clipB, bindB, skeleton, 0.0f, poseB);
    Pose direct(skeleton);
    Blend(poseA, poseB, 0.25f, direct);

    // 4 dan 8 pada bobot 0,25 → 5. Yang diperiksa bukan angkanya saja melainkan
    // bahwa jalur task menghasilkan hal yang sama persis dengan jalur langsung:
    // dua jalur yang berbeda hasilnya adalah dua jalur yang salah satunya tidak
    // pernah diuji.
    CHECK(viaTasks.Local(1).translation.x == doctest::Approx(5.0f));
    for (int bone = 0; bone < skeleton.BoneCount(); ++bone) {
        CHECK(viaTasks.Local(bone).translation.x ==
              doctest::Approx(direct.Local(bone).translation.x));
    }
}

TEST_CASE("akar yang tidak sah menghasilkan bind pose, bukan pose kosong") {
    const Skeleton skeleton = MakeChain(3);
    TaskSystem tasks;
    tasks.Reset(skeleton);

    // Karakter yang runtuh ke titik asal jauh lebih sulit dilacak daripada
    // karakter yang berdiri di bind pose — yang kedua langsung terbaca sebagai
    // "graph-nya tidak menghasilkan apa-apa".
    Pose result;
    CHECK(tasks.Execute(kInvalidTask, result, 0.0f) == false);
    CHECK(result.BoneCount() == skeleton.BoneCount());
    CHECK(result.Local(1).translation.y == doctest::Approx(1.0f));

    const TaskIndex reference = tasks.Register(std::make_unique<ReferencePoseTask>());
    CHECK(tasks.Execute(reference, result, 0.0f));
    CHECK(result.Local(1).translation.y == doctest::Approx(1.0f));
    CHECK(tasks.Pool().InUse() == 0);
}
