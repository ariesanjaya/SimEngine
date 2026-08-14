#pragma once

#include "Sim/Material/MaterialGraph.h"

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sim::material {

/// Versi skema berkas `.simmatinst` yang ditulis sekarang.
inline constexpr int kInstanceSchemaVersion = 1;

/// Nilai sebuah parameter, sudah terurai menjadi angka.
///
/// **Terurai, bukan literal Slang.** `MaterialParameter::defaultValue` disimpan
/// sebagai teks karena berkas `.simmat` harus ramah dibaca dan di-diff, tapi
/// nilai itu tidak pernah masuk kode yang dihasilkan — kompiler hanya menulis
/// nama variabelnya, dan isinya datang dari uniform buffer saat runtime. Jadi
/// yang dibutuhkan instance dan panel adalah angka, dan menguraikannya sekali di
/// satu tempat lebih baik daripada di setiap pemakainya.
struct MaterialValue {
    ValueKind kind = ValueKind::Float;
    std::array<float, 4> components{0.0f, 0.0f, 0.0f, 0.0f};

    bool operator==(const MaterialValue& other) const {
        return kind == other.kind && components == other.components;
    }
};

/// Mengurai literal Slang menjadi angka.
///
/// Menerima bentuk yang benar-benar ditulis di berkas: `0.3`, `float3(0.8)`,
/// `float3(1.0, 0.5, 0.25)`, `float4(1,1,1,1)`. Bentuk yang tidak dikenali
/// menghasilkan nilai netral menurut tipenya — bukan kegagalan: sebuah parameter
/// yang literalnya salah ketik tidak boleh membuat seluruh material gagal dibuka,
/// dan nilai nol jauh lebih mudah dilihat daripada pesan di log.
///
/// Satu angka yang mengisi vektor adalah bentuk yang sah dan disengaja —
/// `float3(0.8)` berarti (0.8, 0.8, 0.8), sama seperti di Slang.
MaterialValue ParseValue(ValueKind kind, std::string_view text);

/// Kebalikannya, dalam bentuk yang bisa ditulis kembali ke `.simmat`.
std::string FormatValue(const MaterialValue& value);

/// Satu parameter yang ditimpa instance.
struct ParameterOverride {
    std::string name;
    MaterialValue value;
};

/// Tekstur yang mengisi sebuah **parameter bertipe Texture** milik induknya.
///
/// **Parameter, bukan node.** Versi pertama medan ini menyebut node
/// `input.texture` lewat GUID-nya, dan itu salah: ia membuat instance bisa
/// menjangkau bagian mana pun dari graph induk, termasuk tekstur yang penulis
/// induk maksudkan sebagai bagian tetap dari definisinya. Yang boleh ditimpa
/// harus **dinyatakan induknya**, sama seperti parameter skalar yang sudah ada
/// — `MaterialGraph::parameters` adalah daftar itu, dan `ResolveParameters`
/// hanya menelusuri yang tercantum di sana.
///
/// Konsekuensinya justru yang diinginkan: induk menjadi **template**. Ia
/// menyatakan "material ini punya satu tekstur albedo", dan setiap instance
/// mengisinya sendiri — satu fungsi material, ratusan tekstur berbeda, tanpa
/// satu byte pun berubah di induknya.
///
/// Terpisah dari `ParameterOverride`, dan bukan karena kerapian: `MaterialValue`
/// adalah empat float, dan sebuah GUID juga enam belas byte — menyelundupkannya
/// lewat medan yang sama akan **kompilasi dengan mulus** lalu menghasilkan
/// tekstur yang berganti setiap kali ada yang menormalkan nilainya atau
/// menuliskannya sebagai `float4(...)`.
struct TextureOverride {
    std::string parameter;
    Uuid texture;
};

/// Material instance: sebuah `.simmat` induk ditambah nilai yang ditimpa.
///
/// **Instance tidak menyalin graph.** Ia hanya menyimpan GUID induk dan daftar
/// parameter yang benar-benar diubah. Dua akibat yang keduanya diinginkan:
/// memperbaiki induk memperbaiki seluruh instance-nya sekaligus, dan berkas
/// instance tetap kecil berapa pun besar graph induknya.
///
/// Yang TIDAK ditimpa tidak ditulis. Itu yang membuat mengubah nilai bawaan di
/// induk mengalir ke instance yang tidak pernah menyentuh parameter itu —
/// perilaku yang sama dengan properti skrip di Inspector, dan bukan kebetulan:
/// keduanya menjawab pertanyaan yang sama.
struct MaterialInstance {
    Uuid parent;
    std::vector<ParameterOverride> overrides;
    /// Tekstur yang mengisi parameter bertipe Texture milik induknya. Yang
    /// tidak disebut memakai bawaan induknya — aturan yang sama dengan
    /// parameter skalar.
    std::vector<TextureOverride> textures;

    const ParameterOverride* Find(std::string_view name) const;
    /// Memasang atau memperbarui satu timpaan.
    void Set(std::string_view name, const MaterialValue& value);
    /// Membuang timpaan sebuah parameter, mengembalikannya ke nilai induk.
    void Clear(std::string_view name);

    /// Tekstur yang mengisi sebuah parameter, atau GUID tak sah.
    ///
    /// **Tidak memeriksa apakah induknya memang menyatakan parameter itu** —
    /// yang memeriksanya `ResolveTextures`, karena hanya ia yang memegang
    /// induknya. Timpaan untuk parameter yang tidak ada tersimpan tanpa
    /// berpengaruh, persis seperti timpaan parameter skalar yang namanya salah
    /// ketik.
    Uuid Texture(std::string_view parameter) const;
    /// Memasangnya. GUID tekstur tak sah membuang timpaan itu — satu jalan
    /// untuk "pakai bawaan induk", bukan dua yang harus dibedakan setiap
    /// pembaca.
    void SetTexture(std::string_view parameter, const Uuid& texture);
};

/// Parameter bertipe Texture beserta aset yang BERLAKU untuknya.
struct ResolvedTexture {
    std::string name;
    std::string tooltip;
    Uuid texture;
    bool overridden = false;
};

/// Parameter sebuah instance beserta nilai yang BERLAKU.
struct ResolvedParameter {
    std::string name;
    ValueKind kind = ValueKind::Float;
    MaterialValue value;
    /// True bila nilainya datang dari instance, bukan dari induk.
    bool overridden = false;
    std::string tooltip;
    float minValue = 0.0f;
    float maxValue = 0.0f;
    bool isColor = false;
};

/// Menggabungkan parameter induk dengan timpaan instance.
///
/// Urutannya mengikuti induk, bukan urutan timpaan: daftar parameter adalah
/// milik material, dan panel yang urutannya berubah-ubah menurut apa yang
/// kebetulan disunting lebih dulu tidak bisa dihafal.
///
/// Timpaan yang parameternya sudah tidak ada di induk dibuang — sama seperti
/// koneksi yang menunjuk node yang hilang. Timpaan yang tipenya tidak lagi cocok
/// juga: sebuah angka yang menjadi warna tidak punya nilai lama yang masuk akal.
std::vector<ResolvedParameter> ResolveParameters(const MaterialGraph& parent,
                                                 const MaterialInstance& instance);

/// Parameter bertipe Texture milik induk, beserta aset yang berlaku.
///
/// **Menelusuri deklarasi induk, bukan daftar timpaan instance.** Itu yang
/// membuat instance tidak bisa menjangkau apa pun yang tidak dinyatakan induknya
/// — aturan yang sama persis dengan `ResolveParameters`, dan bukan kebetulan:
/// keduanya menjawab pertanyaan yang sama untuk tipe yang berbeda.
std::vector<ResolvedTexture> ResolveTextures(const MaterialGraph& parent,
                                             const MaterialInstance& instance);

std::string SaveInstanceToString(const MaterialInstance& instance);
MaterialIoResult SaveInstanceToFile(const MaterialInstance& instance,
                                    const std::filesystem::path& path);

MaterialIoResult LoadInstanceFromString(MaterialInstance& instance, const std::string& text);
MaterialIoResult LoadInstanceFromFile(MaterialInstance& instance,
                                      const std::filesystem::path& path);

}  // namespace sim::material
