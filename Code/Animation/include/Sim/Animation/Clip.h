#pragma once

#include "Sim/Animation/Pose.h"
#include "Sim/Core/Curve.h"

#include <string>
#include <string_view>
#include <vector>

namespace sim::animation {

/// Kanal yang bisa dianimasikan pada sebuah bone.
///
/// **Rotasi disimpan sebagai tiga kurva Euler, bukan sebagai kunci kuaternion.**
/// Ini pemisahan yang disengaja antara bentuk penulisan dan bentuk pemakaian:
///
///  - *Menulis* menuntut kanal skalar. Dope sheet, penyunting kurva bertangen,
///    dan "geser kunci ini 3 frame" semuanya hanya punya arti pada angka
///    tunggal terhadap waktu. Tidak ada penyunting kurva yang bisa menampilkan
///    kuaternion, dan setiap DCC — Maya, Blender, Unity — memperlihatkan Euler
///    kepada animator karena alasan yang sama.
///  - *Memakai* menuntut kuaternion. Pencampuran Euler salah: dua sudut yang
///    dicampur komponen demi komponen tidak menghasilkan rotasi di antaranya.
///
/// Karena itu kurvanya dicuplik menjadi kuaternion **saat sampling**, dan
/// seluruh pencampuran terjadi setelah itu. Yang dibayar: gimbal lock adalah
/// sifat kurva yang ditulis, terlihat dan bisa dikendalikan penulisnya — sama
/// seperti di DCC mana pun. Klip yang kelak diimpor dari FBX/glTF di E8 membawa
/// kunci kuaternion dan akan memerlukan jenis track tersendiri; itu memang
/// jalur yang berbeda, dan menyamakannya sekarang hanya akan salah untuk
/// keduanya.
enum class Channel : uint8_t {
    TranslationX,
    TranslationY,
    TranslationZ,
    RotationX,
    RotationY,
    RotationZ,
    ScaleX,
    ScaleY,
    ScaleZ,
};

inline constexpr int kChannelCount = 9;

const char* ToString(Channel channel);
Channel ChannelFromString(std::string_view text);
/// Kelompok kanal (translasi / rotasi / skala) — dipakai dope sheet untuk
/// menampilkan satu baris per properti alih-alih tiga.
int ChannelGroup(Channel channel);

/// Nilai sebuah kanal pada sebuah transform.
///
/// Dipakai penyunting saat membuat track baru: kunci pertamanya harus bernilai
/// sama dengan bind pose, bukan nol. Track skala yang lahir bernilai nol
/// meruntuhkan bone menjadi titik pada frame pertama juga.
float ChannelValue(const BoneTransform& transform, Channel channel);

/// Satu track: satu kanal pada satu bone.
struct Track {
    /// **Bone ditunjuk lewat nama, bukan indeks.** Indeks bergeser begitu ada
    /// bone disisipkan atau dihapus, dan track yang bergeser menganimasikan
    /// tulang yang salah tanpa ada yang menyadarinya. Nama juga yang membuat
    /// retargeting mungkin sama sekali.
    std::string bone;
    Channel channel = Channel::TranslationX;
    Curve curve;
};

/// Penanda yang memanggil sesuatu saat pemutaran melewatinya.
struct Event {
    float time = 0.0f;
    std::string name;
};

/// Penanda fase, dipakai memadankan dua klip yang dicampur.
///
/// **Tanpa ini, blend jalan↔lari membuat kaki menggeser di tanah.** Dicampur
/// pada waktu ternormalisasi yang sama, dua klip berdurasi berbeda berada pada
/// fase langkah yang berbeda — kaki kiri sedang menapak di satu klip sementara
/// di klip lain sedang terangkat, dan hasilnya kaki yang mengambang. Penanda
/// fase memetakan waktu satu klip ke waktu berfase sama pada klip lain, jadi
/// yang dicampur selalu "menapak dengan menapak".
struct SyncMarker {
    float time = 0.0f;
    std::string name;
};

/// Satu klip animasi.
class Clip {
public:
    std::string name;
    /// Durasi dalam detik. Selalu > 0.
    float duration = 1.0f;
    /// Frame per detik, dipakai dope sheet untuk mengunci kunci ke frame.
    /// Tidak memengaruhi sampling — sampling bekerja pada waktu, bukan frame.
    float frameRate = 30.0f;
    bool looping = true;
    /// Gerakan bone root diangkat keluar dari pose dan diserahkan ke pemanggil.
    ///
    /// Tanpa ini karakter berjalan di tempat sementara kapsul fisikanya diam,
    /// atau ia meluncur karena kecepatan kapsulnya tidak pernah cocok dengan
    /// langkah kakinya. Root motion memindahkan sumber kebenarannya ke klip.
    bool extractRootMotion = false;
    /// Bone yang membawa root motion. Kosong berarti bone pertama rangka.
    std::string rootBone;

    // --- track ---------------------------------------------------------------

    int TrackCount() const { return static_cast<int>(tracks_.size()); }
    const Track& TrackAt(int index) const;
    Track& TrackAt(int index);
    const std::vector<Track>& Tracks() const { return tracks_; }

    /// Mencari track sebuah (bone, kanal), atau -1.
    int FindTrack(std::string_view bone, Channel channel) const;
    /// Mengembalikan track yang ada, atau membuatnya kalau belum ada.
    int EnsureTrack(const std::string& bone, Channel channel);
    bool RemoveTrack(int index);
    void SetTracks(const std::vector<Track>& tracks);
    /// Membuang track yang seluruh kuncinya sama dengan nilai bind — track yang
    /// tidak menganimasikan apa pun tetap berongkos satu evaluasi kurva per
    /// frame.
    int RemoveEmptyTracks();

    // --- event ---------------------------------------------------------------

    const std::vector<Event>& Events() const { return events_; }
    std::size_t AddEvent(const Event& event);
    bool RemoveEvent(std::size_t index);
    void SetEvents(const std::vector<Event>& events);

    /// Event yang dilewati pemutaran maju dari `fromTime` ke `toTime`.
    ///
    /// **Selang setengah terbuka `[from, to)`.** Itu yang membuat sebuah event
    /// menyala tepat sekali per lintasan: dengan selang tertutup ia menyala dua
    /// kali di batas frame, dan dengan selang terbuka event pada waktu 0 tidak
    /// pernah menyala sama sekali. Konsekuensinya event tepat pada `duration`
    /// tidak pernah menyala — dan itu benar, karena pada klip berulang waktu
    /// `duration` adalah waktu 0 lintasan berikutnya.
    ///
    /// Lintasan yang membungkus (klip berulang, `to < from`) dilayani sebagai
    /// dua selang. Frame yang melompati beberapa event menyalakan semuanya,
    /// berurutan — event yang hilang karena frame yang panjang adalah bug yang
    /// hanya muncul di mesin lambat.
    void CollectEvents(float fromTime, float toTime, std::vector<const Event*>& out) const;

    // --- penanda fase ---------------------------------------------------------

    const std::vector<SyncMarker>& SyncMarkers() const { return sync_; }
    std::size_t AddSyncMarker(const SyncMarker& marker);
    bool RemoveSyncMarker(std::size_t index);
    void SetSyncMarkers(const std::vector<SyncMarker>& markers);

    /// Waktu pada `other` yang berfase sama dengan `time` pada klip ini.
    ///
    /// Kalau salah satu klip tidak punya penanda, atau jumlahnya berbeda, ia
    /// jatuh kembali ke waktu ternormalisasi — hasil yang sama dengan tidak
    /// memakai sinkronisasi sama sekali, bukan hasil yang salah diam-diam.
    float MatchPhase(float time, const Clip& other) const;

    // --- waktu ----------------------------------------------------------------

    /// Menjepit atau membungkus waktu sesuai `looping`.
    float WrapTime(float time) const;

private:
    std::vector<Track> tracks_;
    std::vector<Event> events_;
    std::vector<SyncMarker> sync_;
};

/// Pemetaan track sebuah klip ke indeks bone sebuah rangka.
///
/// **Dipisah dari klipnya**, karena satu klip bisa dipasang ke lebih dari satu
/// rangka — dan itu persis yang disebut retargeting. Menyimpan indeks di dalam
/// klip berarti klip yang hanya bisa dipakai satu rig, dan indeks yang basi
/// setiap kali rig itu disunting.
///
/// Pemetaannya dihitung sekali, bukan sekali per frame: mencari nama bone di
/// dalam lingkaran sampling adalah perbandingan string ratusan kali per frame
/// untuk jawaban yang tidak pernah berubah.
class ClipBinding {
public:
    void Bind(const Clip& clip, const Skeleton& skeleton);
    void Bind(const Clip& clip, const Skeleton& skeleton, const RetargetMap& retarget);

    /// Indeks bone untuk sebuah track, atau -1 kalau rangkanya tidak punya.
    int BoneFor(int track) const;
    /// Track yang tidak menemukan bone-nya. Dipakai panel untuk memperlihatkan
    /// apa yang hilang saat sebuah klip dipasang ke rig yang salah — diam soal
    /// itu berarti animasi yang setengah berjalan tanpa sebab yang terlihat.
    const std::vector<std::string>& Unresolved() const { return unresolved_; }
    int RootBone() const { return rootBone_; }
    bool Empty() const { return boneForTrack_.empty(); }

private:
    std::vector<int> boneForTrack_;
    std::vector<std::string> unresolved_;
    int rootBone_ = -1;
};

/// Mencuplik klip menjadi pose.
///
/// Bone yang tidak disentuh track mana pun tetap pada bind pose rangkanya.
void SampleClip(const Clip& clip, const ClipBinding& binding, const Skeleton& skeleton,
                float time, Pose& out);

/// Perpindahan root di antara dua waktu, sudah memperhitungkan pembungkusan.
///
/// Dikembalikan sebagai **delta**, bukan sebagai posisi mutlak: yang dibutuhkan
/// pemanggil adalah "sejauh apa karakter bergerak sejak frame lalu" supaya bisa
/// diteruskan ke kapsul fisika, dan menghitung selisih dari dua posisi mutlak di
/// sisi pemanggil akan salah tepat pada frame tempat klipnya berulang.
BoneTransform SampleRootMotion(const Clip& clip, const ClipBinding& binding,
                               const Skeleton& skeleton, float fromTime, float toTime);

}  // namespace sim::animation
