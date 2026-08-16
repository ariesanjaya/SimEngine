// Menyalin gambar yang sudah dirender ke gambar swapchain.
//
// **Ini titik akhir, bukan sebuah efek.** Perender menggambar ke targetnya
// sendiri karena editor menampilkan hasilnya di dalam sebuah panel; di player
// tidak ada panel, jadi seseorang harus memindahkannya ke layar. Di editor yang
// melakukannya ImGui, dan pass ini yang menggantikannya untuk yang tidak punya
// ImGui.
//
// Tidak ada tonemap di sini, dan itu disengaja: perender sudah menyerahkan
// warna yang siap tampil. Menonemap lagi berarti gambar yang sama terlihat
// berbeda antara editor dan player — selisih yang akan dicari orang di
// materialnya.

struct Push {
    /// Petak sumber yang benar-benar terisi, sebagai pecahan alokasinya.
    ///
    /// Target render dialokasikan lebih besar daripada yang diminta supaya
    /// menyeret pemisah dock tidak mengalokasi ulang tiap frame. Segitiga
    /// penutup layar menghasilkan uv 0..1 atas **layar**, jadi tanpa skala ini
    /// yang tersalin adalah seluruh alokasi — termasuk sisa yang tidak pernah
    /// diisi, yang tampil sebagai pita hitam di kanan dan bawah.
    float2 sourceUvScale;
};

[[vk::push_constant]] ConstantBuffer<Push> pc;
[[vk::binding(0, 0)]] Sampler2D uSource;

struct Varyings {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("fragment")]
float4 main(Varyings input) : SV_Target {
    return float4(uSource.Sample(input.uv * pc.sourceUvScale).rgb, 1.0);
}
