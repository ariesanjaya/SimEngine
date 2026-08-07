#pragma once

#include "Sim/Animation/Skeleton.h"

#include <cstdint>
#include <vector>

namespace sim::animation {

/// Satu pose: transform lokal tiap bone.
///
/// **Lokal, bukan global.** Pose global tidak bisa dicampur: mencampur dua pose
/// global mengabaikan hierarkinya, jadi lengan yang dicampur akan lepas dari
/// bahu yang juga sedang dicampur. Global dihitung sekali di ujung rantai, saat
/// pencampuran sudah selesai.
class Pose {
public:
    Pose() = default;
    /// Pose sama dengan bind pose rangkanya. Ini keadaan awal yang benar untuk
    /// hampir semua hal: bone yang tidak disentuh klip mana pun harus berdiri di
    /// bind pose, bukan di titik nol.
    explicit Pose(const Skeleton& skeleton);

    void Reset(const Skeleton& skeleton);
    /// Mengubah jumlah bone **tanpa membuang isi yang masih muat**.
    ///
    /// Bedanya penting: `Blend` memanggil ini pada keluarannya, dan keluarannya
    /// kerap pose yang sama dengan salah satu masukannya — runtime graph
    /// mencampur lapis demi lapis ke dalam satu pose hasil. Kalau ini
    /// mengosongkan isinya lebih dulu, masukan itu sudah terhapus sebelum
    /// sempat dibaca, dan hasilnya pose kosong yang terlihat sebagai karakter
    /// yang runtuh.
    void Resize(int boneCount);

    int BoneCount() const { return static_cast<int>(local_.size()); }
    const BoneTransform& Local(int index) const;
    BoneTransform& Local(int index);
    const std::vector<BoneTransform>& Locals() const { return local_; }
    std::vector<BoneTransform>& Locals() { return local_; }

    /// Transform global tiap bone, satu lintasan maju.
    void ComputeGlobal(const Skeleton& skeleton, std::vector<BoneTransform>& out) const;
    /// Matriks kulit: global × invers bind. Yang diunggah perender.
    void ComputeSkinning(const Skeleton& skeleton, std::vector<Mat4>& out) const;

    /// Buffer kerja milik sampling: sudut Euler sementara, dan penanda kanal
    /// rotasi mana yang sudah ditulis sebuah track.
    ///
    /// **Tinggal di pose, bukan di dalam `SampleClip`.** Mencuplik klip terjadi
    /// puluhan kali per detik per karakter, dan dua `std::vector` lokal di sana
    /// berarti dua alokasi per pencuplikan — kriteria "klip 60 detik bisa
    /// di-scrub mulus" pada dasarnya kriteria tentang tidak mengalokasi di jalur
    /// itu. Pose adalah milik satu instance dan tidak pernah dibagi antar-thread,
    /// jadi ia tempat yang aman untuk menyimpannya.
    std::vector<Vec3>& EulerScratch(std::size_t boneCount);
    std::vector<uint8_t>& TouchedScratch(std::size_t boneCount);

private:
    std::vector<BoneTransform> local_;
    std::vector<Vec3> eulerScratch_;
    std::vector<uint8_t> touchedScratch_;
};

/// Bobot per bone, 0..1. Kosong berarti "seluruh bone berbobot penuh".
///
/// Disimpan **per bone dan bukan sebagai daftar nama**, karena ia dibaca sekali
/// per bone per frame di jalur pencampuran; mencari nama di sana adalah pencarian
/// string di dalam lingkaran terdalam. Namanya dipetakan sekali saat mask dipasang
/// ke sebuah rangka.
class BoneMask {
public:
    void Reset(int boneCount, float weight = 1.0f);
    void Clear() { weights_.clear(); }
    bool Empty() const { return weights_.empty(); }
    int BoneCount() const { return static_cast<int>(weights_.size()); }

    float Weight(int bone) const;
    void SetWeight(int bone, float weight);
    /// Menetapkan bobot sebuah bone beserta seluruh keturunannya — bentuk yang
    /// hampir selalu dimaksud orang saat menulis "mask dari pinggang ke atas".
    void SetChainWeight(const Skeleton& skeleton, int root, float weight);

    const std::vector<float>& Weights() const { return weights_; }

private:
    std::vector<float> weights_;
};

/// Mencampur dua pose: `weight` 0 menghasilkan `a`, 1 menghasilkan `b`.
///
/// **`out` boleh pose yang sama dengan `a` atau `b`.** Runtime graph
/// mengandalkannya: ia mencampur lapis demi lapis ke dalam satu pose hasil, dan
/// menyediakan pose antara untuk tiap lapis berarti satu alokasi per lapis per
/// frame. Aman karena tiap bone dibaca sebelum ditulis, dan karena `Resize`
/// tidak membuang isi yang masih muat.
///
/// Rotasi dicampur dengan nlerp yang dipendekkan, bukan lerp komponen: dua
/// kuaternion yang mewakili rotasi berdekatan bisa berlawanan tanda, dan
/// mencampurnya apa adanya membuat karakter berputar lewat jalan yang panjang.
/// Nlerp, bukan slerp: pada bobot yang berubah mulus keduanya tidak bisa
/// dibedakan mata, sedangkan slerp membawa dua fungsi trigonometri per bone per
/// frame.
void Blend(const Pose& a, const Pose& b, float weight, Pose& out);
/// Sama, dengan bobot dikalikan mask per bone. Mask kosong berarti penuh.
void Blend(const Pose& a, const Pose& b, float weight, const BoneMask& mask, Pose& out);

/// Menambahkan pose `additive` yang dinyatakan relatif terhadap `reference`.
///
/// Dipakai layer aditif — mis. napas atau recoil yang harus berlaku di atas
/// gerakan apa pun yang sedang berjalan, bukan menggantikannya.
void BlendAdditive(const Pose& base, const Pose& additive, const Pose& reference, float weight,
                   const BoneMask& mask, Pose& out);

}  // namespace sim::animation
