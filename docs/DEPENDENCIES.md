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
yang sama: `ufbx` yang dijadwalkan E8 membaca FBX, sedangkan OBJ menuntut
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
| **ufbx** / **cgltf** | impor mesh & animasi (FBX seperti `Defeated.fbx`, dan glTF) |
| **meshoptimizer** | optimisasi vertex cache, generasi LOD, simplifikasi |
| **PhysX 5** | fisika — sumber lokal di `/home/arie/SDK/PhysX-main` |
| **OpenAL Soft** | audio — sumber lokal di `/home/arie/SDK/openal-soft-1.25.2` |
| **Tracy** | profiler (opsional, `SIM_WITH_TRACY`) |
| **slang** | bahasa shader (sudah tersedia sebagai `slangc` di Vulkan SDK) |

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
