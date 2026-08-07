#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Animation/AnimationIo.h"
#include "Sim/Animation/Clip.h"
#include "Sim/Animation/Pose.h"
#include "Sim/Animation/Skeleton.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
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
