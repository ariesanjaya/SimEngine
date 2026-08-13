# Dependensi

Semua dependensi diambil lewat `FetchContent` di `cmake/SimDeps.cmake` dengan tag
versi pasti. Tidak ada dependensi yang diambil dari sistem kecuali **Vulkan SDK**
(sudah ada di `/home/arie/SDK/vulkan-sdk-1.4.350.1`).

Setelah konfigurasi pertama, `FETCHCONTENT_UPDATES_DISCONNECTED=ON` membuat build
berikutnya tidak menyentuh jaringan. Untuk build sepenuhnya offline setelah itu:
`-DFETCHCONTENT_FULLY_DISCONNECTED=ON`.

## Inti (dipakai sejak E0/E1)

| Paket | Versi terkunci | Untuk apa | Catatan |
|---|---|---|---|
| **SDL3** | `release-3.4.12` | jendela, input, monitor, surface Vulkan | Shared lib. Dependensi opsionalnya (XScrnSaver, ALSA, PulseAudio, PipeWire, JACK, sndio, Wayland) diprobe pkg-config di `SimDeps.cmake` dan dimatikan bila paket dev-nya tidak ada — tanpa itu SDL menggagalkan konfigurasi di mesin bersih |
| **Dear ImGui** | `v1.92.9b-docking` | seluruh UI editor | **Wajib tag `-docking`** — di sinilah docking + multi-viewport + `ConfigDpiScaleFonts` berada |
| **volk** | tag SDK 1.4.350 | loader fungsi Vulkan | `IMGUI_IMPL_VULKAN_USE_VOLK` |
| **VulkanMemoryAllocator** | `v3.4.0` | alokasi memori GPU | dipakai `RHI::Buffer`/`Image` |
| **glm** | `1.0.3` | matematika | di-alias jadi `sim::Vec3` dst supaya bisa diganti |
| **spdlog** | `v1.17.0` | logging | sink file + sink ring-buffer untuk panel Console |
| **nlohmann/json** | `v3.12.0` | serialisasi semua aset & level | format teks supaya ramah diff/git |
| **EnTT** | `v3.16.0` | storage komponen scene | 3.x, bukan 4.0 — dokumentasi & contoh jauh lebih matang |
| **doctest** | `v2.5.3` | unit test | header-only, kompilasi cepat |
| **stb** | commit terkunci | `stb_image` untuk tekstur & thumbnail, `stb_image_write` untuk kompresor deflate-nya, `stb_image_resize2` untuk thumbnail | header-only; implementasinya dikompilasi tepat sekali di `Third-Party/stb/stb_impl.cpp` (target `Stb::Impl`) |

## UI tambahan (E4 ke atas)

| Paket | Versi | Untuk apa | Risiko |
|---|---|---|---|
| **ImGuizmo** | commit `5ab7676` (master) | gizmo translate/rotate/scale di viewport | Kompatibilitas dengan ImGui 1.92 belum terjamin. Dibungkus `Sim::Gizmo`; kalau pecah, tulis sendiri (≈600 baris). |
| **imgui-node-editor** | commit `021aa0e` (master) + patch | graph visual scripting (E6.5), Material (E7.1) & Animation state machine (E7.5) | Dibungkus `Sim::NodeGraph`. Dipatok ke `master`, bukan `develop`: keduanya mendahului ImGui 1.92, tapi hanya `master` yang memuat penggantinya untuk `ImRect::Floor()` dan `ImGui::GetKeyIndex()`. Dua patch kecil di `cmake/patches/`: `operator*(float, ImVec2)` yang kini disediakan ImGui sendiri, dan area klik latar kanvas yang melewatkan tombol menu konteks — cacat yang tersembunyi selama tombol pan dan tombol menu kebetulan sama. |
| **IconFontCppHeaders** | vendored di `Third-Party/` | konstanta codepoint ikon | Di-vendor, bukan di-fetch: codepoint di header harus cocok persis dengan `.ttf` di `Resources/Fonts` |

## Aset yang dibundel (bukan lewat FetchContent)

| Aset | Versi | Lisensi | Lokasi |
|---|---|---|---|
| **Inter** | 4.1 | SIL OFL 1.1 (`Resources/Fonts/Inter-LICENSE.txt`) | `Resources/Fonts/Inter-{Regular,Medium,SemiBold}.ttf` |
| **Lucide** | 1.28.0 | ISC (`Resources/Fonts/lucide-LICENSE.txt`) | `Resources/Fonts/lucide.ttf` |
| **ShaderBall** | derkreature/ShaderBall (Maret 2015), model oleh Mat Makin | Unlicense / public domain (`Resources/Meshes/shaderBall-LICENSE.txt`) | `Resources/Meshes/shaderBall.fbx` |

Shader ball dibundel sebagai **aset contoh**, bukan sebagai bagian engine: ia
disemai ke folder aset proyek saat proyek itu belum punya folder `Meshes`, lalu
dipakai entity "Shader Ball" di level contoh. FBX, bukan OBJ yang ada di sumber
yang sama: FBX SDK yang dipakai E8 membaca FBX, sedangkan OBJ menuntut
pembaca tersendiri yang tidak ada di rencana mana pun. Ia juga lebih kecil di
git meski lebih besar di disk — 1,6 MB terkompresi berbanding 1,8 MB.

Berkas `.meta`-nya ikut dibundel. GUID aset tinggal di sana, jadi tanpa itu
setiap mesin membangkitkan GUID sendiri dan berkas level yang merujuk model ini
hanya berlaku di mesin tempat ia dibuat.

Font UI dibundel, bukan diambil dari sistem. Alasannya bukan estetika semata:
metrik font menentukan lebar kolom dan tinggi baris, jadi memakai font sistem
berarti tata letak panel bergeser antar-mesin dan berkas layout tersimpan tidak
lagi cocok. Inter dipilih karena dirancang untuk teks UI kecil — tinggi-x besar
dan angka bertabular, yang membuat kolom X/Y/Z di Inspector tidak bergoyang saat
nilainya berubah. `Resources/` disalin ke folder keluaran saat build.

Lucide dipilih sebagai set ikon karena gayanya berbasis garis dengan ketebalan
yang sepadan dengan Inter; ikon solid akan tampil lebih "berat" daripada teks di
sebelahnya. Ikon di-merge ke ImFont yang sama dengan teks, sehingga bisa ditulis
langsung di tengah string biasa (lihat `Sim/Editor/Icons.h`).

> **Jebakan yang sudah kena sekali.** Inter memetakan 745 glyph ke Private Use
> Area — wilayah yang sama dengan tempat font ikon berada. Saat merge, sumber
> pertama yang memiliki sebuah codepoint yang menang, sehingga sebagian ikon
> diam-diam tergantikan glyph Latin milik Inter (`chevron-down` menjadi "3",
> `save` menjadi "9"). `ImGuiLayer` karena itu memasang
> `ImFontConfig::GlyphExcludeRanges` pada font teks untuk rentang ikon. Kalau
> suatu saat font teks diganti, pengecualian ini harus tetap ada.

Widget kurva, gradient, timeline, dan dialog file **ditulis sendiri** di
`EditorFramework/Widgets`. Alasannya: keempatnya dipakai lintas editor (partikel,
animasi, terrain) dengan kebutuhan yang spesifik, dan pustaka pihak ketiga untuk
ini umumnya tidak terawat.

## AI / MCP (track A)

| Paket | Versi | Untuk apa | Catatan |
|---|---|---|---|
| **cpp-httplib** | `v0.51.0` | server HTTP localhost untuk MCP (A0) dan klien HTTPS ke Claude API (A4) | Header tunggal. HTTPS butuh OpenSSL sistem (`CPPHTTPLIB_OPENSSL_SUPPORT`), hanya diaktifkan untuk sisi klien |
| **nlohmann/json** | (sudah ada) | JSON-RPC 2.0 dan JSON Schema tool | dipakai ulang dari daftar inti |

Protokol MCP dan JSON-RPC 2.0 **diimplementasi sendiri** di `Code/AIBridge` —
sekitar 600 baris. Alasannya: SDK MCP resmi untuk C++ belum ada, dan permukaan
protokol yang kita butuhkan (`initialize`, `tools/*`, `resources/*`, `prompts/*`)
kecil dan stabil.

## Scripting (E6)

| Paket | Versi | Catatan |
|---|---|---|
| **Lua** | `v5.4.8` | Repo resmi tidak punya CMake; kita sediakan `cmake/lua.CMakeLists.txt` sendiri |
| **sol2** | `v3.5.0` | Header-only. `SOL_ALL_SAFETIES_ON=1` di Debug, dimatikan di Release |

## Fase rendering & runtime (E8/E9)

Ditambahkan saat dibutuhkan, dicatat di sini supaya keputusannya tidak diulang:

| Paket | Untuk apa |
|---|---|
| **cgltf** | impor glTF. FBX SDK sudah masuk di E8.4 dan membaca FBX serta OBJ |
| **Autodesk FBX SDK** | impor FBX (mesh, rangka, klip). **Dicari, tidak diunduh, dan wajib** — lihat di bawah |
| **meshoptimizer** | optimisasi vertex cache, generasi LOD, simplifikasi |
| **OpenUSD** | impor `.usd/.usda/.usdc/.usdz`. **Opsional dan dicari, tidak diunduh** — lihat di bawah |
| **tinyexr** | impor OpenEXR untuk IBL (I2). **Opsional** (`SIM_WITH_EXR`), satu header lewat FetchContent |
| **libtiff** | impor/ekspor heightmap TIFF 16/32-bit (I3). **Opsional** (`SIM_WITH_TIFF`), dicari di sistem |
| **OpenVDB** | bake mesh → SDF untuk clipmap GI (V1), dan nanti impor `.vdb`. **Opsional** (`SIM_WITH_OPENVDB`), dicari, tidak dibangun — lihat di bawah |
| **OpenImageIO** | backend gambar yang **didahulukan bila ada** — DDS, PSD utuh, metadata colorspace. **Opsional** (`SIM_WITH_OIIO`), dicari, tidak diunduh — lihat di bawah |
| **PhysX 5** | fisika — sumber lokal di `/home/arie/SDK/PhysX-main` |
| **OpenAL Soft** | audio — sumber lokal di `/home/arie/SDK/openal-soft-1.25.2` |
| **Tracy** | profiler (opsional, `SIM_WITH_TRACY`) |
| **slang** | bahasa shader (sudah tersedia sebagai `slangc` di Vulkan SDK) |

### Autodesk FBX SDK — wajib, dan tidak bisa diambil sendiri

Impor FBX — mesh, rangka, dan klip animasi — memakai FBX SDK resmi Autodesk.
**Ia satu-satunya kebergantungan berlisensi milik**, dan itu menetapkan tiga hal
yang tidak berlaku untuk yang lain di berkas ini:

- **Tidak bisa diambil FetchContent dan tidak boleh divendor ke repo ini.**
  Pengunduhannya menuntut penerimaan perjanjian lisensi Autodesk, per orang.
- **Biner yang menautnya tunduk pada perjanjian itu.** Ini keputusan distribusi,
  bukan sekadar keputusan teknis.
- **Tidak ada jalur cadangan.** Tanpa SDK-nya, konfigurasi berhenti dengan pesan
  yang menyebut alasannya — berbeda dengan OpenUSD di bawah, yang dilewati diam
  bila tidak ada. FBX adalah format yang dipakai aset contoh engine ini sendiri
  (`Resources/Meshes/shaderBall.fbx`), jadi build tanpanya tidak akan berguna.

Pasang SDK-nya (Linux, x64) lalu arahkan build ke sana:

```bash
# Pemasang dari Autodesk, dijalankan sekali per mesin.
./fbx20203_fbxsdk_linux ~/SDK/fbxsdk
```

Bawaannya menunjuk `$HOME/SDK/fbxsdk`; folder lain disetel dengan
`-DSIM_FBXSDK_ROOT=<folder>`. Baris `Autodesk FBX SDK dipakai dari ...` pada
keluaran CMake menandakan ia ditemukan.

Yang ditautkan adalah `lib/release/libfbxsdk.a` yang **statis**, bukan `.so`-nya.
Yang dinamis menuntut `libfbxsdk.so` ikut ditemukan saat dijalankan, dan
satu-satunya tempat ia tinggal adalah folder pemasangan milik satu mesin — biner
yang dipindah lalu berhenti bekerja dengan pesan dari loader, bukan dari mesin
ini. Yang statis menyeret libxml2 dan zlib, dan keduanya memang ada di mana-mana.

> **Membaca berkas tidak boleh menulis apa pun.** Bawaan SDK membongkar media
> tertanam ke sebuah folder `<nama>.fbm/` di sebelah berkas FBX-nya — di dalam
> folder aset milik orang lain — lalu menulis ulang jalur tekstur supaya menunjuk
> ke sana. `IMP_FBX_EXTRACT_EMBEDDED_DATA` karena itu dimatikan di
> `MeshImport.cpp`.

### OpenUSD — dicari, tidak dibangun

Semua kebergantungan lain diambil lewat FetchContent karena masing-masing
berbiaya detik sampai satu menit. OpenUSD bukan salah satunya: ia menyeret
oneTBB, dan membangun keduanya dari nol memakan puluhan menit dan ratusan
megabyte. Menjadikannya wajib berarti setiap orang yang hanya ingin mengubah
satu panel editor membayar biaya itu pada build bersihnya yang pertama.

Jadi ia dipakai kalau ada dan dilewati kalau tidak. Build tanpanya tetap utuh;
yang hilang hanya impor USD, dan berkas `.usd` yang dibuka di sana ditolak
dengan alasan yang menyebut mesinnya — bukan dengan "format tidak dikenal" yang
mengirim orang memeriksa berkasnya.

Menyediakannya, sekali per mesin:

```bash
# oneTBB — headernya tidak ikut paket runtime distro
git clone --depth 1 --branch v2022.0.0 https://github.com/uxlfoundation/oneTBB.git
cmake -S oneTBB -B tbb-build -DCMAKE_BUILD_TYPE=Release -DTBB_TEST=OFF \
      -DCMAKE_INSTALL_PREFIX=$HOME/SDK/usd -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build tbb-build --target install -j

# OpenUSD, monolitik dan tanpa Python/imaging — yang dipakai importir hanya
# pembaca panggungnya
git clone --depth 1 --branch v25.05 \
      https://github.com/PixarAnimationStudios/OpenUSD.git
cmake -S OpenUSD -B usd-build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=$HOME/SDK/usd -DCMAKE_PREFIX_PATH=$HOME/SDK/usd \
      -DPXR_ENABLE_PYTHON_SUPPORT=OFF -DPXR_BUILD_IMAGING=OFF \
      -DPXR_BUILD_USD_IMAGING=OFF -DPXR_BUILD_USDVIEW=OFF \
      -DPXR_BUILD_TESTS=OFF -DPXR_BUILD_EXAMPLES=OFF -DPXR_BUILD_TUTORIALS=OFF \
      -DPXR_BUILD_USD_TOOLS=OFF -DPXR_BUILD_MONOLITHIC=ON \
      -DPXR_ENABLE_MATERIALX_SUPPORT=OFF -DPXR_ENABLE_GL_SUPPORT=OFF \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build usd-build --target install -j
```

Lalu konfigurasikan SimEngine dengan `-DSIM_USD_ROOT=$HOME/SDK/usd`. Baris
`OpenUSD ditemukan:` pada keluaran CMake menandakan impor USD ikut terbangun.

#### Kompiler yang membangun USD ikut menentukan ABI-nya

`pxr/base/tf/hashmap.h` memilih induk `TfHashMap` lewat
`ARCH_HAS_GNU_STL_EXTENSIONS`, dan `pxr/base/arch/defines.h` menyalakan makro itu
hanya pada Linux **dengan GCC**. Dibangun GCC, `TfHashMap` mewarisi
`__gnu_cxx::hash_map` dan besarnya 40 bait; header yang sama dibaca Clang
mewarisi `std::unordered_map` dan besarnya 56. Setiap kelas USD yang menyimpan
`TfHashMap` karena itu punya offset anggota yang berbeda di kedua sisi.

Yang tidak cocok tidak gagal saat ditautkan — ia jatuh belakangan, sebagai
segfault di dalam destruktor USD yang di-inline ke pemanggilnya, pada berkas
`.usda` sekecil satu kubus.

Ini tidak perlu diurus sendiri: SimDeps.cmake membaca cabang mana yang dipakai
langsung dari tabel simbol pustakanya dan menyamakan sisi pemanggil. Yang perlu
diketahui hanya bahwa **paket USD tidak bisa dipertukarkan begitu saja** — yang
dibangun Clang dan yang dibangun GCC bukan barang yang sama, dan menukar isi
`SIM_USD_ROOT` tanpa mengonfigurasi ulang membuat build memakai kesimpulan lama.

### Backend gambar — tiga lapis, satu wajib

`Sim::ImageIO` memilih backend per ekstensi, dan yang lebih mampu menang:

| Lapis | Opsi | Format | Wajib? |
|---|---|---|---|
| **stb** | — | PNG, JPEG, TGA, BMP, HDR, PSD terbatas. Satu-satunya yang **menulis** PNG greyscale 8/16-bit | **ya** |
| **tinyexr** | `SIM_WITH_EXR` | `.exr` | tidak |
| **libtiff** | `SIM_WITH_TIFF` | `.tif`/`.tiff`, baca dan tulis | tidak |
| **OpenImageIO** | `SIM_WITH_OIIO` | semuanya di atas + `.dds`, PSD utuh, metadata colorspace | tidak |

Build bersih hanya butuh stb — tujuh format, nol setup. Dengan tinyexr dan
libtiff (`apt install libtiff-dev zlib1g-dev`) menjadi sepuluh. Dengan OIIO,
sebelas, dan `.psd` serta metadata colorspace ikut benar.

```bash
sudo apt install libtiff-dev zlib1g-dev
```

Aturan ruang warna dan alfa yang berlaku untuk setiap tekstur ada di
[TEXTURE-CONVENTIONS.md](TEXTURE-CONVENTIONS.md).

#### OpenImageIO — didahulukan bila ada, tidak pernah wajib

**Yang ditambahkannya** ketika hadir: impor `.dds` untuk aset lama, PSD yang
utuh alih-alih composited view 8/16-bit saja, metadata colorspace untuk PNG dan
JPEG (yang tidak dibaca stb sama sekali), dan `ImageBufAlgo` untuk pembandingan
gambar di I5.

**Kenapa tidak dijadikan syarat.** Biayanya terukur: **396 MB** arsip statik yang
harus disalin dengan tangan, ditambah rantai yang tidak disebut paketnya di mana
pun — boost 1.78 (×4), pugixml, dan fmt 10.2.1. Paket OIIO yang dibawa
distribusi OpenUSD memasang `detail/fmt/format.h` sebagai shim satu baris yang
meneruskan ke `<fmt/format.h>`, jadi ia memang tidak bisa dipakai dari luar
tanpa fmt sendiri; dan versinya tidak bebas dipilih karena fmt 12 yang dibawa
spdlog membuang hal-hal yang masih dipakai header OIIO 2.5. Menjadikannya syarat
berarti SimEngine kehilangan sifat "bisa dibangun hanya dengan FetchContent dan
Vulkan SDK".

**Root sendiri, bukan menumpang paket OpenUSD.** Distribusi OpenUSD kebetulan
membawa header OIIO, dan menaruhnya di sana berarti header itu masuk include
path setiap target yang menautkan `Usd::Usd` — termasuk target yang tidak boleh
melihatnya. `Third-Party/OpenImageIO/` menutup itu secara struktural.

**Header saja tidak cukup.** Paket OpenUSD membawa headernya tanpa pustakanya,
dan kalau hanya header yang ada `#include <OpenImageIO/imageio.h>` kompilasi
dengan sukses lalu gagal saat link. SimDeps memeriksa keduanya dan melewati
backend ini — dengan pesan yang menyebut sebabnya — bila hanya headernya ada.

Menyediakannya, sekali per mesin:

```bash
# Berkasnya ada di paket OpenUSD yang sama yang mengisi Third-Party/OpenUSD.
# Sesuaikan $USD_PKG dengan paket yang dipakai.
USD_PKG=$HOME/.cache/packman/chk/usd.py310.manylinux_2_35_x86_64.stock.release/0.25.05.post.2-gl.15479+b8a1f40c
D=Third-Party/OpenImageIO
mkdir -p "$D/include" "$D/lib"
cp -a "$USD_PKG"/include/OpenImageIO "$USD_PKG"/include/Imath "$D/include/"
cp -a "$USD_PKG"/lib/libOpenImageIO.a "$USD_PKG"/lib/libOpenImageIO_Util.a "$D/lib/"
for b in filesystem thread atomic system; do cp -a "$USD_PKG"/lib/libboost_$b.so* "$D/lib/"; done

# Pustaka pendukung dari distro; sonamenya cocok dengan yang dipakai saat
# arsipnya dibangun, jadi tidak perlu disalin ke dalam pohon ini.
sudo apt install libtiff-dev libjpeg-dev libpng-dev zlib1g-dev libpugixml-dev
```

Baris `OpenImageIO 2.5.18 dipakai dari ...` pada keluaran CMake menandakan
backendnya ikut terbangun. **boost datang dari paket yang sama, bukan dari
distro**: arsipnya dibangun terhadap boost 1.78 dan boost tidak menjanjikan ABI
stabil antar-versi.

**Bukan karena OpenUSD.** Layak diluruskan karena mudah dikira begitu: USD tidak
pernah mendekode gambar. Ia merujuk tekstur sebagai jalur aset eksternal, dan
kaitan USD dengan OIIO hidup di **Hydra/Hio** — `hioOiio` adalah plugin
`libusd_hio`, bagian imaging. Paket USD yang dipakai di sini tidak membawa
imaging sama sekali, dan tidak satu pun dari 20 pustaka USD yang di-vendor
merujuk simbol OIIO. Alasan memakai OIIO adalah kemampuan gambarnya, bukan
integrasi USD-nya.

**Tindihan antar-backend itu disengaja.** Ketika OIIO ada, ia membaca format yang
sama dengan ketiga backend lain — dan `SimImageIOTests` memanfaatkannya: setiap
fixture dibaca lewat **setiap** backend yang mengaku bisa, lalu dituntut identik
bit per bit. Itulah yang mengadu penyusunan ulang kanal EXR dan pembacaan strip
TIFF yang ditulis di sini dengan implementasi yang sudah dipakai seluruh
industri.

### OpenVDB — dicari, tidak dibangun

Dua kegunaan:

1. **Bake mesh → SDF** untuk clipmap GI, melengkapi M1 di
   [rencana-implementasi-gi.md](rencana-implementasi-gi.md) yang menyebut "bake
   SDF per-mesh offline" tapi belum pernah ada.
2. **Impor `.vdb`** — asap, api, dan awan dari Houdini atau EmberGen — menjadi
   grid padat yang bisa diunggah sebagai tekstur 3D.

**Opsional.** Yang melewatinya tetap membangun seluruh mesin; clipmap GI mundur
ke `BoxSceneField` — jarak ke kotak berorientasi per mesh, yang memang sudah
dipakai sampai sekarang. Bedanya nyata: untuk bola berjari-jari 1, kotak
pembungkusnya menyebut titik di arah diagonal berjarak **negatif** padahal ia
jelas di luar bolanya.

**Pengondisi aset, bukan pustaka runtime** — aturan yang sama dengan
OpenImageIO. Yang keluar dari `Sim::Volume` adalah `sim::SdfGrid`: float biasa
di `Sim::Core`, tanpa satu pun tipe OpenVDB. `Sim::Render` memakai hasilnya dan
tidak pernah menautkan pustakanya.

Membangunnya sekali per mesin, lalu menyalin hasilnya ke dalam pohon:

```bash
sudo apt install libtbb-dev libblosc-dev libboost-iostreams-dev zlib1g-dev

git clone --depth 1 --branch v13.0.0 https://github.com/AcademySoftwareFoundation/openvdb.git
cmake -S openvdb -B vdb-build -DCMAKE_BUILD_TYPE=Release \
      -DOPENVDB_BUILD_BINARIES=OFF -DOPENVDB_BUILD_PYTHON_MODULE=OFF \
      -DOPENVDB_BUILD_UNITTESTS=OFF
cmake --build vdb-build -j

# Header sumber, lalu version.h hasil generate ditaruh **di sebelahnya**.
D=Third-Party/OpenVDB
mkdir -p "$D/include" "$D/lib"
cp -a openvdb/openvdb/openvdb "$D/include/openvdb"
cp -a vdb-build/openvdb/openvdb/openvdb/version.h "$D/include/openvdb/"
find "$D/include" \( -name "*.cc" -o -name "CMakeLists.txt" \) -delete
cp -a vdb-build/openvdb/openvdb/libopenvdb.so* "$D/lib/"
```

**`version.h` tidak ada di pohon sumber** — ia dihasilkan saat OpenVDB dibangun,
dan `Types.h` meng-include-nya dengan tanda kutip. Salinan header dari pohon
sumber saja karena itu gagal dikompilasi dengan pesan yang tidak menyebut
sebabnya, jadi SimDeps memeriksa keberadaannya sendiri dan mengatakan apa yang
kurang.

Baris `OpenVDB 13.0.0 dipakai dari ...` pada keluaran CMake menandakan bake ikut
terbangun.

### PhysX 5 — dicari, tidak dibangun

Simulasi rigid body, scene query, joint, artikulasi, dan kendaraan untuk E9.
Rencananya di [PLAN-PHYSICS.md](PLAN-PHYSICS.md).

**Opsional, dan ketiadaannya terbaca.** Yang melewatinya tetap membangun seluruh
mesin, tetapi tidak ada yang jatuh: `PhysicsWorld::Create` menolak dan
`Error()`-nya menyebut PhysX. Itu disengaja — **benda diam yang seharusnya
bergerak adalah gejala paling mahal untuk didiagnosis**, karena ia terlihat persis
seperti bug simulasi. Log startup editor karena itu selalu menyebutkan status
fisika, ada maupun tidak.

**Pustaka runtime, bukan pengondisi aset** — dan di situ ia berbeda dari
OpenImageIO dan OpenVDB, yang keduanya berhenti sesudah aset selesai diolah.
Aturan seam-nya tetap sama dan diuji, bukan sekadar disepakati: tidak satu pun
tipe `Px*` boleh keluar dari `Code/Physics/src/PhysicsWorld.cpp`, dan
`SimPhysicsTests` menyisir seluruh `Code/` untuk membuktikannya — bentuk yang
sama dengan uji `stbi_` di `SimImageIOTests`.

**Dibangun CPU-only.** PBD, soft body FEM, dan deformable surface menuntut CUDA
lewat tanda tangan API-nya sendiri (`PxCudaContextManager&`, bukan pointer), jadi
ketiganya tidak punya jalur CPU untuk dimundurkan. Preset `linux-clang-cpu-only`
menghasilkan pustaka statis tanpa satu pun ketergantungan CUDA, dan itu yang
disalin ke dalam pohon:

```bash
git clone --depth 1 --branch 106.6-physx-5.6.1 https://github.com/NVIDIA-Omniverse/PhysX.git
cd PhysX/physx
./generate_projects.sh linux-clang-cpu-only
cmake --build compiler/linux-clang-cpu-only-release -j

# Header lengkap — termasuk PxConfig.h yang baru ada sesudah generate — lalu
# delapan pustaka statis CPU. Yang GPU sengaja tidak ikut.
D=Third-Party/PhysX
mkdir -p "$D/include" "$D/lib"
cp -a include/. "$D/include/"
cp -a compiler/linux-clang-cpu-only-release/sdk_source_bin/PxConfig.h "$D/include/"
for lib in PhysX PhysXCommon PhysXFoundation PhysXExtensions PhysXCooking \
           PhysXCharacterKinematic PhysXPvdSDK PhysXVehicle2; do
    cp -a bin/linux.x86_64/release/lib${lib}_static_64.a "$D/lib/"
done
```

**Urutan taut penting.** Linker GNU membaca arsip statis kiri ke kanan dan hanya
sekali, jadi `PhysXExtensions` harus mendahului `PhysX`, yang mendahului
`PhysXFoundation`. Urutan yang salah tidak muncul sebagai peringatan melainkan
sebagai simbol yang tidak ditemukan padahal berkasnya jelas ada di daftar taut;
`SimDeps.cmake` sudah menyusunnya dan tidak perlu diubah.

Baris `PhysX 5.6.1 dipakai dari ... — simulasi aktif (CPU)` pada keluaran CMake
menandakan simulasinya ikut terbangun. `-DSIM_WITH_PHYSX=OFF` mematikannya;
`-DSIM_WITH_PHYSX_GPU=ON` menuntut pustaka GPU yang tidak ikut disalin di atas,
dan baru relevan pada P8.

## Yang sengaja tidak dipakai

Dicatat supaya keputusannya tidak ditimbang ulang tiap kali namanya muncul.

| Kandidat | Untuk | Kenapa tidak |
|---|---|---|
| **EmotionFX** (O3DE) | sistem animasi E7.5 | Bukan pustaka lepas melainkan sebuah *Gem*: setiap Gem diturunkan dari `AZ::Module` milik AzCore, jadi memakainya berarti ikut membawa refleksi, EBus, dan `AZ::Data::Asset` milik O3DE — ketiganya bertabrakan langsung dengan `Sim::Reflect`, `AssetDatabase`, dan scene EnTT yang sudah ada. Editornya juga Qt, sedangkan panel di sini Dear ImGui, jadi sisi editornya tetap harus ditulis ulang. Lisensinya (Apache-2.0/MIT) bukan penghalangnya; kopel arsitekturnya yang jadi penghalang. |
| **ozz-animation** | runtime sampling/blending E7.5 | MIT dan benar-benar lepas, tapi ia menyatakan dirinya *bukan* blend tree tingkat tinggi — yang disediakannya matematika sampling/blending/IK, sementara isi terbesar E7.5 justru state machine, transisi berkondisi, dan blend tree. Format binernya juga melawan pola aset `.sim*` JSON + berkas pendamping. Layak ditimbang ulang di E8 kalau impor rig ternyata berat. |
| **Esoterica** (`/home/arie/SDK/Esoterica`) | sistem animasi E7.5 | MIT dan tooling-nya Dear ImGui — kandidat terdekat yang pernah ditimbang — tapi tidak bisa dipakai di sini: `Code/Base/Platform/` hanya berisi Win32 (tidak ada satu pun jalur POSIX di kodenya sendiri), build-nya MSBuild tanpa CMake sama sekali, dan `Engine/Animation` terkopel ke `Base` (362 ribu baris) lewat sistem resource, `Time`, `Quantization`, serta refleksi `EE_REFLECT_TYPE`/`EE_SERIALIZE` yang dibangkitkan generator libclang. Memakainya berarti mem-port sebuah engine ke Linux + CMake lebih dulu. |

**Esoterica tetap dipakai sebagai acuan desain**, bukan dependensi — lisensinya
mengizinkan membaca dan mengadaptasi dengan atribusi, dan README-nya memang
memposisikan diri begitu. Dua konsep diambil dari sana ke dalam lingkup E7.5 yang
tadinya tidak menyebutnya: **sync track** (blending tersinkron, tanpanya blend
jalan↔lari membuat kaki menggeser di tanah karena dua klip dicampur pada fase
langkah yang berbeda) dan **root motion**.

## Toolchain terverifikasi

Diperiksa pada 2 Agustus 2026 di mesin ini:

```
clang       18.1.3 (Ubuntu)
cmake       3.28.3
ninja       1.11.1
Vulkan SDK  1.4.350.1  (/home/arie/SDK/vulkan-sdk-1.4.350.1)
            glslc, glslangValidator, slangc, dxc tersedia di x86_64/bin
```

`CMakePresets.json` menetapkan `CMAKE_C_COMPILER=clang` dan
`CMAKE_CXX_COMPILER=clang++` secara eksplisit di semua preset.

Sebelum konfigurasi, sumber environment Vulkan SDK:

```sh
source /home/arie/SDK/vulkan-sdk-1.4.350.1/setup-env.sh
```

## Referensi lokal yang dipakai (bukan dependensi)

| Path | Dipakai untuk |
|---|---|
| `/home/arie/SDK/SimEngine/sdl3_vulkan.cpp` | Acuan langsung setup SDL3 + Vulkan + ImGui di E1 |
| `/home/arie/SDK/Vulkan-Tutorial` | Acuan konsep Vulkan (swapchain, pipeline, descriptor) untuk E8 |
| `/home/arie/SDK/PhysX-main` | Sumber PhysX untuk E9 |
| `/home/arie/SDK/o3de` | Acuan alur kerja & tata letak editor (gambar acuan berasal dari sini) |
| `/home/arie/SDK/MemeEngine` | Proyek sebelumnya — acuan konvensi CMake, dokumen, dan struktur modul |
| `/home/arie/SDK/openpbr.slang` | Implementasi OpenPBR Surface v1.1 real-time. Menentukan himpunan parameter node keluaran Material Editor (E7.1) dan model shading E8. Nilai bawaannya sudah dicocokkan dengan berkas normatif OpenPBR (lihat baris di bawah) dan dikunci test |
| [`reference/open_pbr_surface.mtlx`](https://github.com/AcademySoftwareFoundation/OpenPBR) | Berkas **normatif** nilai bawaan OpenPBR Surface (nodedef MaterialX, `isdefaultversion`). Yang menang kalau berbeda dari halaman prosa spesifikasinya — halaman itu memuat sisa revisi lama, dan sempat membuat `coat_roughness`/`coat_ior` terbaca 0.03/1.5 alih-alih 0.0/1.6 |
| `/home/arie/SDK/rencana-implementasi-gi.md` | Rencana global illumination: `ITraceBackend` dengan dua implementasi (SDF clipmap / ray query), screen probe, hash grid radiance cache. Anggaran 3,0 ms @ 1080p, baseline GTX 1660 Super. Belum masuk roadmap — lihat catatan di PLAN-RENDER.md |
| [`adobe/openpbr-bsdf`](https://github.com/adobe/openpbr-bsdf) | Implementasi acuan OpenPBR 1.1 dari Adobe. **Untuk path tracing, bukan real time** — README-nya menyatakan "designed for unidirectional path tracing", dan isinya evaluate + sample + PDF, 7 tabel kompensasi energi, integrasi volumetrik, sampling panjang gelombang. Dipakai sebagai acuan kebenaran, bukan sebagai kode yang dipakai ulang |
| `/home/arie/SDK/openpbr_dfg.slang` | Generator LUT split-sum (DFG) untuk berkas di atas. RG16F 128², `E_spec = F0*dfg.x + dfg.y`; deterministik, jadi boleh di-bake sekali dan di-ship |
| `/home/arie/SDK/atmosphere-bac` | Hamburan atmosfer & awan volumetrik untuk langit E8 ([MatejSakmary](https://github.com/MatejSakmary/atmosphere-bac), Apache-2.0, skripsi S1 beserta [teksnya](https://github.com/MatejSakmary/atmosphere-bac-text)). Vulkan + GLSL, CMake + preset, dikembangkan di Linux — jadi ia bisa dibaca *dan* dijalankan di mesin ini, tidak seperti acuan yang hanya bisa dibaca. Isinya rangkaian LUT model Bruneton yang memang dipakai engine mana pun: `transmittanceLUT` → `multiscatteringLUT` → `skyviewLUT` → `aerialPerspectiveLUT`, lalu awan, terrain, dan komposisi akhir dengan histogram eksposur. Aset teksturnya tidak ikut di repo (diunduh terpisah), jadi menjalankannya butuh langkah itu dulu |
