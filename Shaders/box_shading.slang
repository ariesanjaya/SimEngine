// Badan tahap fragment kubus, dipakai bersama kedua varian jalur tekstur.
//
// **Dipisah karena yang berbeda antara keduanya hanya satu baris**, dan satu
// baris bukan alasan yang cukup untuk dua salinan seluruh pencahayaan. Yang
// menyertakan berkas ini wajib sudah mendeklarasikan:
//
//     float4 boxBaseColor(BoxVarying input)
//
// yaitu albedo ruas ini — dari descriptor set material pada jalur mundur, dan
// dari larik tekstur bindless pada jalur bindless.
//
// Seluruh varying diserahkan, bukan hanya uv-nya: jalur bindless mengambil nomor
// slot teksturnya dari sana, dan jalur mundur tidak memakainya sama sekali —
// perbedaan yang memang harus tinggal di dalam fungsi itu.
//
// Sengaja tanpa include guard: ia memang badan sebuah entry point, dan menyertakannya
// dua kali dalam satu modul adalah dua `main` — yang harus menjadi galat, bukan
// didiamkan oleh guard.

[shader("fragment")]
float4 main(BoxVarying input) : SV_Target {
    // Pencahayaan sementara: satu arah plus ambient. Bukan model shading —
    // OpenPBR masuk lewat pipeline material, dan menulis pendekatan kedua di
    // sini melanggar aturan yang dipegang seluruh E7.1: model shading hanya
    // boleh punya satu implementasi. Ini sekadar supaya bentuk kotaknya terbaca.
    //
    // Bayangannya sungguhan, dan itu bukan ketidakkonsistenan: yang sedang
    // diuji di sini adalah pass bayangannya, dan ia harus terlihat pada
    // geometri yang sudah ada sebelum pipeline material siap memakainya.
    float3 normal = normalize(input.normal);
    float3 lightDir = normalize(shadowParams.lightDirection.xyz);
    float ndotl = max(dot(normal, lightDir), 0.0);
    // Yang tidak menerima bayangan tetap dihitung n·l-nya — ia tersinari
    // seperti biasa, hanya tidak pernah tergelapkan oleh yang lain.
    float shadow = (input.flags & kFlagReceiveShadows) != 0u
                       ? sampleShadow(input.worldPosition, normal)
                       : 1.0;
    float viewDepth = dot(input.worldPosition - shadowParams.cameraPosition.xyz,
                          shadowParams.cameraForward.xyz);
    // `punctual` adalah iradiansi E(x). Ia dijumlahkan ke suku ad-hoc di
    // sebelahnya tanpa dibagi pi — dan itu konsisten hanya karena seluruh
    // ekspresi ini memang ad-hoc. Jalur yang benar (albedo/pi * E) ada di
    // openpbr.slang, dan ke sanalah lampu punctual pindah begitu pipeline
    // material menggantikan shader ini.
    float3 punctual =
        accumulateClusteredLights(input.position.xy, input.worldPosition, normal, viewDepth);
    // **Iradiansi tak-langsung datang dari langit, bukan dari angka tetap.**
    // Sampai B1 suku ini adalah konstanta 0,25 — penambal supaya bentuk kotaknya
    // terbaca — dan konstanta itulah yang membuat menukar seluruh langit tidak
    // menggerakkan pencahayaan permukaan satu tingkat pun. Sekarang ia SH yang
    // dipanggang dari langit yang benar-benar tergambar, dan ia ikut bergeser
    // saat mataharinya bergeser.
    //
    // Dua sumber, satu pemilik: SH panggang **atau** probe, tidak pernah
    // keduanya. Menjumlahkannya menghasilkan adegan terang dua kali lipat tanpa
    // satu pun galat di log — keputusan 2 di docs/PLAN-IBL.md.
    //
    // Dibagi pi karena keduanya mengembalikan iradiansi E, sedangkan yang
    // dijumlahkan di sini radiansi keluar permukaan Lambert ber-albedo
    // `input.color`. Kesalahan pi tidak pernah terlihat salah — ia hanya membuat
    // seluruh adegan terlalu terang sebesar faktor tetap, yang paling mudah
    // dikira masalah eksposur.
    float3 indirect = skyIrradiance(normal) / 3.14159265;
    if (shadowParams.giParams.x > 0.5) {
        indirect = giIrradianceAt(input.position.xy, input.worldPosition, normal,
                                  shadowParams.giParams.y) / 3.14159265;
    }
    // Radiance matahari datang dari scene sekarang — warna dikali intensitas
    // dikali eksposur — bukan lagi angka 0,75 yang ditulis di sini.
    // **Dikalikan, bukan menggantikan.** Ruas tanpa tekstur membawa putih, dan
    // putih tidak mengubah apa pun — jadi setiap mesh yang sudah ada tergambar
    // persis seperti sebelum jalur ini ada.
    float4 albedo = boxBaseColor(input);
    float3 surface = input.color.rgb * albedo.rgb;
    // Mode clay: satu albedo untuk semuanya, cahayanya utuh. Di jalur ini itu
    // saja yang perlu diganti — `box.frag` tidak punya spekular, logam, maupun
    // peta normal yang bisa ikut mengaburkan bentuk bayangan.
    //
    // **Warna instance ikut dibuang, bukan tetap dikalikan.** Ia yang membedakan
    // warna kotak satu dari kotak lain, dan mode ini ada justru supaya tidak ada
    // beda warna yang bisa dikira beda pencahayaan.
    if (clayView()) {
        surface = kClayAlbedo;
    }

    // **Adegan tanpa lampu digambar diffuse-nya apa adanya.**
    //
    // Sampai sekarang ia tersinari matahari bawaan `ViewportDesc` — cahaya yang
    // tidak dimiliki entity mana pun, tidak muncul di Outliner, dan tidak bisa
    // dimatikan. Menghapus lampu terakhir sebuah level karena itu tidak mengubah
    // apa pun di layar, dan yang mencari dari mana cahayanya datang tidak akan
    // menemukannya.
    //
    // Yang menggantikannya bukan layar hitam. Hitam memang jujur — tanpa cahaya
    // tidak ada yang dipantulkan — tapi ia menghapus satu-satunya hal yang masih
    // bisa dikerjakan orang di adegan yang belum punya lampu: melihat bentuk dan
    // warna yang sudah ia susun. Diffuse datar menjawab keduanya: tidak ada
    // cahaya yang dikarang, dan tidak ada yang hilang dari layar.
    //
    // **Mode Unlit masuk lewat cabang yang sama.** Yang diminta sakelar viewport
    // persis yang sudah dilakukan cabang ini — albedo datar, tanpa cahaya yang
    // dikarang — dan menuliskannya sebagai jalur kedua berarti dua definisi
    // "tanpa cahaya" yang suatu saat berselisih.
    if (unlitView() || !anyLightInScene()) {
        return float4(surface, input.color.a * albedo.a);
    }

    float3 lit = surface *
                 (indirect + shadowParams.sunRadiance.rgb * (ndotl * shadow) + punctual);
    return float4(lit, input.color.a * albedo.a);
}
