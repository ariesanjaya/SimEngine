// Operator nada ACES dan encode sRGB.
//
// **Angkanya harus sama persis dengan `Code/Render/src/ToneMap.cpp`.** Yang di
// CPU adalah acuan yang diuji; yang di sini yang benar-benar terlihat. Kurva
// nada yang berbeda antara keduanya membuat setiap uji lulus dan setiap gambar
// salah.

/// Matriks ACES ditulis sebagai baris dan dipakai sebagai hasil kali titik per
/// baris. **Bukan `mul`:** matriks yang tertranspos di sini tidak menghasilkan
/// galat apa pun — hanya seluruh gambar yang bergeser rona, sedikit, dan
/// pergeseran sekecil itu tidak akan pernah dicurigai sebagai bug matriks.
float3 acesInput(float3 v) {
    return float3(dot(v, float3(0.59719, 0.35458, 0.04823)),
                  dot(v, float3(0.07600, 0.90834, 0.01566)),
                  dot(v, float3(0.02840, 0.13383, 0.83777)));
}

float3 acesOutput(float3 v) {
    return float3(dot(v, float3(1.60475, -0.53108, -0.07367)),
                  dot(v, float3(-0.10208, 1.10813, -0.00605)),
                  dot(v, float3(-0.00327, -0.07276, 1.07602)));
}

float acesCurve(float value) {
    float a = value * (value + 0.0245786) - 0.000090537;
    float b = value * (0.983729 * value + 0.4329510) + 0.238081;
    return clamp(a / b, 0.0, 1.0);
}

float3 acesToneMap(float3 color) {
    float3 v = acesInput(color);
    float3 fitted = float3(acesCurve(v.x), acesCurve(v.y), acesCurve(v.z));
    return clamp(acesOutput(fitted), 0.0, 1.0);
}

/// Encode sRGB.
///
/// **Wajib di sini, dan tidak dilakukan siapa pun sebelumnya.** Swapchain-nya
/// UNORM, bukan SRGB — sengaja, karena ImGui menggambar UI dengan warna yang
/// sudah dalam ruang sRGB. Akibatnya tidak ada satu pun tempat lain yang
/// meng-encode, dan sebelum pass ini viewport menampilkan nilai linier apa
/// adanya: seluruh adegan lebih gelap dan lebih kontras daripada yang dimaksud,
/// merata, sehingga tidak terlihat sebagai kesalahan melainkan sebagai gaya.
float3 linearToSrgb(float3 color) {
    float3 low = color * 12.92;
    float3 high = 1.055 * pow(max(color, 0.0031308), 1.0 / 2.4) - 0.055;
    return select(color <= 0.0031308, low, high);
}

float luminanceOf(float3 color) {
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}
