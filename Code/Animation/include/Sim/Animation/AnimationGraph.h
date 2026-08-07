#pragma once

#include "Sim/Animation/Clip.h"
#include "Sim/Core/AssetRef.h"

#include <string>
#include <string_view>
#include <vector>

namespace sim::animation {

// --- bobot blend tree ---------------------------------------------------------

/// Bobot tiap simpul blend tree 1D pada sebuah nilai parameter.
///
/// **Hanya dua simpul yang pernah aktif sekaligus**, yaitu dua yang mengapit
/// nilainya. Itu bukan penyederhanaan melainkan yang diharapkan orang dari
/// sumbu satu dimensi: pada kecepatan 3 m/s di antara "jalan" (2) dan "lari"
/// (5), tidak ada alasan "diam" (0) ikut menyumbang. Di luar simpul terluar,
/// bobotnya ditahan pada simpul terluar itu — bukan diekstrapolasi, karena
/// tidak ada klip di sana untuk diekstrapolasi.
///
/// `positions` tidak harus terurut; urutan keluarannya mengikuti urutan
/// masukannya.
void Blend1DWeights(const std::vector<float>& positions, float value,
                    std::vector<float>& out);

/// Bobot tiap simpul blend tree 2D, dengan interpolasi gradient band.
///
/// **Bukan sekadar "yang terdekat" dan bukan jarak terbalik.** Yang terdekat
/// menghasilkan lompatan di batas antar-simpul; jarak terbalik membuat setiap
/// simpul menyumbang di mana-mana, jadi berdiri tepat di atas simpul "lari"
/// tetap mencampurkan sedikit "mundur". Gradient band memberi keduanya sekaligus:
/// tepat di sebuah simpul bobotnya persis 1 dan yang lain persis 0, di sepanjang
/// ruas antara dua simpul ia linear, dan peralihannya mulus di mana pun.
///
/// Untuk tiap simpul i, bobotnya adalah minimum atas seluruh j ≠ i dari
/// `1 − ((p − pᵢ)·(pⱼ − pᵢ)) / |pⱼ − pᵢ|²`, dijepit ke [0,1], lalu seluruhnya
/// dinormalkan. Titik di luar sebaran simpul jatuh ke simpul terluar ke arah
/// itu, bukan menjadi nol semua.
void Blend2DWeights(const std::vector<Vec2>& positions, const Vec2& value,
                    std::vector<float>& out);

// --- parameter ----------------------------------------------------------------

enum class ParameterType : uint8_t {
    Bool,
    Float,
    /// Sekali nyala lalu padam sendiri setelah dibaca — lihat `ConsumeTriggers`.
    Trigger,
};

const char* ToString(ParameterType type);
ParameterType ParameterTypeFromString(std::string_view text);

struct Parameter {
    std::string name;
    ParameterType type = ParameterType::Float;
    /// Nilai awal. Bool dan Trigger memakai 0/1.
    float value = 0.0f;
};

/// Nilai parameter yang menggerakkan sebuah graph.
///
/// **Trigger padam sendiri, bool tidak.** Perbedaannya menentukan siapa yang
/// bertanggung jawab: bool adalah keadaan ("sedang jongkok") yang dimiliki
/// gameplay dan hanya gameplay yang boleh mematikannya; trigger adalah kejadian
/// ("lompat sekarang") yang dinyalakan sekali dan harus padam begitu graph
/// melihatnya. Tanpa pemadaman itu, satu tombol lompat membuat karakter melompat
/// selamanya.
class ParameterSet {
public:
    int Count() const { return static_cast<int>(parameters_.size()); }
    const Parameter& At(int index) const;
    Parameter& At(int index);
    const std::vector<Parameter>& All() const { return parameters_; }

    int Add(const Parameter& parameter);
    bool Remove(int index);
    void SetAll(const std::vector<Parameter>& parameters);
    int Find(std::string_view name) const;

    float Float(int index) const;
    bool Bool(int index) const;
    void SetFloat(int index, float value);
    void SetBool(int index, bool value);
    /// Menyalakan sebuah trigger. Tidak berpengaruh pada tipe lain.
    void Fire(int index);

    void SetFloat(std::string_view name, float value);
    void SetBool(std::string_view name, bool value);
    void Fire(std::string_view name);

    /// Mematikan seluruh trigger. Dipanggil runtime **setelah** transisi
    /// dievaluasi, bukan sebelumnya: dipanggil sebelumnya, trigger yang
    /// dinyalakan gameplay pada frame yang sama tidak pernah sempat terlihat.
    void ConsumeTriggers();

private:
    std::vector<Parameter> parameters_;
};

// --- kondisi transisi ---------------------------------------------------------

enum class Comparison : uint8_t {
    Greater,
    Less,
    GreaterEqual,
    LessEqual,
    Equal,
    NotEqual,
};

const char* ToString(Comparison comparison);
Comparison ComparisonFromString(std::string_view text);

/// Satu syarat pada sebuah transisi.
///
/// Kondisi pada satu transisi digabung dengan **DAN**, bukan ATAU. Bukan karena
/// DAN lebih berguna, tapi karena ATAU sudah tersedia tanpa menambah apa pun:
/// dua transisi dari state yang sama ke tujuan yang sama adalah ATAU-nya. Satu
/// bentuk yang bisa menyatakan keduanya lebih baik daripada dua bentuk yang
/// harus dipilih.
struct Condition {
    std::string parameter;
    Comparison comparison = Comparison::Greater;
    float value = 0.0f;
};

/// Menguji sebuah kondisi terhadap nilai parameter yang berlaku.
///
/// Tipe parameternya yang menentukan artinya: Float dibandingkan sebagai angka,
/// Bool dibandingkan sebagai 0/1, dan Trigger benar semata kalau ia sedang
/// menyala — perbandingan pada trigger diabaikan, karena "trigger lebih besar
/// dari 0,5" tidak berarti apa pun bagi siapa pun yang menulisnya.
bool Evaluate(const Condition& condition, const ParameterSet& parameters);

// --- motion -------------------------------------------------------------------

enum class MotionKind : uint8_t {
    Clip,
    Blend1D,
    Blend2D,
};

const char* ToString(MotionKind kind);
MotionKind MotionKindFromString(std::string_view text);

/// Satu klip di dalam sebuah blend tree.
struct MotionChild {
    AssetRef clip;
    /// Kedudukan pada sumbu blend. `x` saja untuk 1D.
    Vec2 position{0.0f, 0.0f};
    float speed = 1.0f;
};

/// Sumber pose sebuah state: satu klip, atau sebuah blend tree.
///
/// **Blend tree tidak bisa disarangkan di dalam blend tree.** Batas yang
/// disengaja: pohon bersarang menuntut evaluasi rekursif beserta bobot yang
/// dikalikan berjenjang, dan hampir seluruh gunanya sudah tercakup oleh blend
/// tree 2D — yang memang alasan 2D ada. Kalau nanti benar-benar dibutuhkan, ia
/// ditambahkan sebagai jenis motion tersendiri, bukan dengan menjadikan seluruh
/// evaluasi rekursif demi kasus yang jarang.
struct Motion {
    MotionKind kind = MotionKind::Clip;
    /// Dipakai `MotionKind::Clip`.
    AssetRef clip;
    /// Parameter sumbu blend tree. `parameterY` hanya dipakai 2D.
    std::string parameterX;
    std::string parameterY;
    std::vector<MotionChild> children;
    float speed = 1.0f;
    /// Klip di dalam blend tree dipadankan lewat penanda fase, bukan lewat
    /// waktu ternormalisasi. Dimatikan kalau klipnya memang tidak sefase —
    /// mis. mencampur "diam" dengan "melambai".
    bool syncPhase = true;
};

// --- state machine ------------------------------------------------------------

struct State {
    std::string name;
    Motion motion;
    /// Letak simpul di kanvas penyunting. Disimpan bersama modelnya, bukan di
    /// berkas terpisah: tata letak yang hilang saat berkasnya dipindahkan
    /// membuat graph yang sudah dirapikan harus dirapikan ulang.
    Vec2 canvas{0.0f, 0.0f};
};

/// Perpindahan antar-state.
struct Transition {
    /// Indeks state asal, atau -1 untuk "dari state mana pun".
    int from = -1;
    int to = 0;
    /// Lama crossfade, detik.
    float duration = 0.2f;
    /// Menunggu klip asal mencapai fraksi tertentu sebelum boleh berpindah.
    bool hasExitTime = false;
    float exitTime = 1.0f;
    /// Digabung dengan DAN. Kosong berarti hanya `exitTime` yang menahannya —
    /// dan transisi tanpa keduanya berpindah seketika, yang memang arti yang
    /// masuk akal untuk "sambungkan A ke B".
    std::vector<Condition> conditions;
};

/// Satu lapis animasi yang dicampurkan di atas lapis di bawahnya.
struct Layer {
    std::string name = "Base";
    float weight = 1.0f;
    /// Aditif: hasil lapis ini ditambahkan sebagai selisih terhadap pose acuan,
    /// bukan menggantikan. Dipakai napas, recoil, dan sejenisnya.
    bool additive = false;
    /// Pangkal rantai bone yang dipengaruhi lapis ini. Kosong berarti seluruh
    /// rangka.
    std::string maskRootBone;

    std::vector<State> states;
    std::vector<Transition> transitions;
    int defaultState = 0;
};

/// Graph animasi: parameter, lapis, state, dan transisinya.
class AnimationGraph {
public:
    ParameterSet parameters;

    int LayerCount() const { return static_cast<int>(layers_.size()); }
    const Layer& LayerAt(int index) const;
    Layer& LayerAt(int index);
    const std::vector<Layer>& Layers() const { return layers_; }

    int AddLayer(const Layer& layer);
    bool RemoveLayer(int index);
    void SetLayers(const std::vector<Layer>& layers);

    /// Rangka yang menjadi acuan graph ini. Kosong berarti belum ditetapkan.
    AssetRef skeleton;
    std::string name;

private:
    std::vector<Layer> layers_;
};

}  // namespace sim::animation
