#pragma once

#include "Sim/Core/Math.h"

#include <string>
#include <string_view>
#include <vector>

namespace sim::animation {

/// Batas jumlah bone.
///
/// Bukan batas penyimpanan melainkan batas yang jujur: setiap bone tambahan
/// adalah satu matriks per instance per frame di buffer skinning, dan tidak ada
/// perender yang mengunggah ribuan matriks per karakter. Kriteria terima E7.5
/// menyebut rig 100 bone; 256 memberi ruang untuk rig wajah tanpa berpura-pura
/// tak berbatas.
inline constexpr int kMaxBones = 256;

/// Transform sebuah bone terhadap induknya.
///
/// **Disimpan terurai (translasi, rotasi, skala), bukan sebagai matriks.**
/// Matriks tidak bisa di-blend: menginterpolasi dua matriks rotasi secara
/// komponen menghasilkan matriks yang bukan rotasi lagi — ia menyusut dan
/// menceng di tengah transisi. Seluruh guna sistem animasi ada pada
/// pencampuran, jadi bentuk yang tidak bisa dicampur bukan pilihan.
struct BoneTransform {
    Vec3 translation{0.0f, 0.0f, 0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f, 1.0f, 1.0f};

    Mat4 ToMatrix() const;
};

/// Menggabungkan dua transform: `parent` lalu `child` di ruang induknya.
BoneTransform Concatenate(const BoneTransform& parent, const BoneTransform& child);

/// Sudut Euler XYZ intrinsik (radian) menjadi kuaternion.
///
/// Urutannya ditulis eksplisit, tidak menyerahkannya ke `glm::quat(vec3)`:
/// konvensi Euler glm tidak sama dengan yang dipakai kurva rotasi di sini, dan
/// urutan yang salah menghasilkan animasi yang *hampir* benar — jenis kesalahan
/// yang paling lama tidak ketahuan.
Quat EulerToQuat(const Vec3& radians);
/// Kebalikannya. Dipakai penyunting untuk memperlihatkan bind pose sebagai
/// angka yang bisa diketik.
Vec3 QuatToEuler(const Quat& rotation);

struct Bone {
    std::string name;
    /// Indeks induk, atau -1 untuk root. **Selalu lebih kecil daripada indeks
    /// bone ini sendiri** — lihat catatan pada `Skeleton`.
    int parent = -1;
    /// Transform terhadap induk pada bind pose.
    BoneTransform bind;
};

/// Rangka: pohon bone beserta bind pose-nya.
///
/// **Bone disimpan berurutan topologis — induk selalu mendahului anaknya.**
/// Itu bukan kerapian melainkan yang membuat penghitungan pose global menjadi
/// satu lintasan maju tanpa rekursi dan tanpa penelusuran ulang: saat sampai di
/// bone ke-i, transform global induknya pasti sudah dihitung. Rig 100 bone
/// dihitung 60 kali per detik, dan penelusuran pohon berulang di jalur itu
/// adalah ongkos yang tidak perlu ada sama sekali.
///
/// Invarian itu dijaga di satu tempat — `AddBone` menolak induk yang indeksnya
/// tidak lebih kecil, dan `RemoveBone` menyusun ulang indeks yang bergeser.
/// Invarian yang dijaga oleh setiap pemanggil adalah invarian yang batal pada
/// pemanggil pertama yang lupa.
class Skeleton {
public:
    int BoneCount() const { return static_cast<int>(bones_.size()); }
    const Bone& Bone(int index) const;
    animation::Bone& Bone(int index);
    const std::vector<animation::Bone>& Bones() const { return bones_; }

    /// Menambah bone. Mengembalikan indeksnya, atau -1 kalau penuh, namanya
    /// kosong, atau induknya tidak sah.
    int AddBone(const animation::Bone& bone);
    /// Menghapus bone **beserta seluruh keturunannya**. Menghapus induk tanpa
    /// anaknya akan meninggalkan bone yang menunjuk induk yang tidak ada, dan
    /// tidak ada arti yang masuk akal untuk itu.
    bool RemoveBone(int index);
    /// Mengganti seluruh isi. Urutan topologisnya diperiksa; daftar yang
    /// melanggar ditolak seluruhnya, bukan sebagian.
    bool SetBones(const std::vector<animation::Bone>& bones);
    void Clear();

    int Find(std::string_view name) const;
    /// Indeks anak langsung sebuah bone.
    void Children(int index, std::vector<int>& out) const;
    /// Apakah `candidate` keturunan `ancestor` (atau sama dengannya).
    bool IsDescendant(int candidate, int ancestor) const;

    /// Transform global tiap bone pada bind pose.
    const std::vector<BoneTransform>& GlobalBind() const;
    /// Kebalikan matriks bind global — yang dipakai skinning. Dihitung sekali
    /// lalu disimpan: ia hanya berubah kalau rangkanya sendiri berubah, dan
    /// menghitungnya ulang tiap frame adalah 100 inversi matriks untuk hasil
    /// yang sama persis.
    const std::vector<Mat4>& InverseBindMatrices() const;

private:
    void Invalidate();

    std::vector<animation::Bone> bones_;
    mutable std::vector<BoneTransform> globalBind_;
    mutable std::vector<Mat4> inverseBind_;
    mutable bool cacheValid_ = false;
};

/// Pemetaan nama bone antar-rig.
///
/// **Retargeting di sini tidak lebih dari sebuah kamus nama**, dan itu memang
/// sudah cukup untuk yang dijanjikan E7.5: klip yang ditulis untuk satu rig
/// dipasang ke rig lain yang proporsinya berbeda tapi hierarkinya sepadan.
/// Yang *tidak* dilakukannya adalah menyesuaikan panjang tulang — klip yang
/// dipetakan ke rig berlengan lebih panjang akan menggerakkan lengan itu dengan
/// sudut yang sama, bukan mencapai titik yang sama. Menyesuaikan jangkauan
/// menuntut IK, dan IK adalah node tersendiri, bukan sifat kamusnya.
class RetargetMap {
public:
    /// Memetakan nama bone di klip → nama bone di rangka tujuan.
    void Set(const std::string& sourceName, const std::string& targetName);
    void Remove(const std::string& sourceName);
    void Clear();
    bool Empty() const { return entries_.empty(); }

    /// Nama tujuan untuk sebuah nama sumber. Nama yang tidak dipetakan
    /// dikembalikan apa adanya — kamus kosong karena itu berarti "nama harus
    /// sama persis", yang memang keadaan lazimnya.
    std::string_view Resolve(std::string_view sourceName) const;

    const std::vector<std::pair<std::string, std::string>>& Entries() const { return entries_; }

private:
    /// Vektor terurut, bukan `unordered_map`: isinya puluhan pasang, ia harus
    /// ditulis ke berkas dengan urutan yang tetap supaya menyimpan dua kali
    /// menghasilkan byte yang sama, dan pencarian biner pada puluhan entri
    /// tidak kalah cepat dari hash.
    std::vector<std::pair<std::string, std::string>> entries_;
};

/// Menyusun kamus dari rig sumber ke rig tujuan lewat **nama rig standar**.
///
/// Setiap rangka mencatat pemetaan nama bone-nya sendiri ke nama standar
/// ("Hips", "LeftUpLeg", ...). Itu yang membuat N rig hanya menuntut N kamus,
/// bukan N² — dan itulah arti "retarget mapping ke rig standar". Kamus langsung
/// antar-dua-rig masih bisa ditulis tangan kalau memang perlu; fungsi ini
/// menyusun yang lazimnya cukup.
///
/// Nama sumber yang standarnya tidak dimiliki rig tujuan **tidak diikutkan**,
/// bukan diteruskan apa adanya: nama yang lolos begitu saja akan kebetulan cocok
/// dengan bone yang tidak ada hubungannya, dan animasi yang menyasar diam-diam
/// lebih buruk daripada anggota badan yang jelas tidak bergerak.
RetargetMap ComposeRetarget(const RetargetMap& sourceToStandard,
                            const RetargetMap& targetToStandard);

}  // namespace sim::animation
