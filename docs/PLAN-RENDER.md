# Plan Rendering & Runtime (E8 → E9)

Fase ini baru dimulai setelah E7 selesai. Ditulis sekarang supaya keputusan yang
dibuat di fase editor tidak menutup pintu ke arah yang kita tuju.

## Apa yang sudah tersedia saat E8 dimulai

Dari fase editor kita sudah punya, dan sudah teruji dipakai:

- `RHI` Vulkan yang bekerja: instance, device, swapchain, VMA, render target
  offscreen, descriptor allocator, frame in-flight (E1).
- Pipeline kompilasi shader GLSL/Slang → SPIR-V yang terhubung ke build (E0).
- Format aset yang stabil: material graph, definisi partikel, terrain, vegetasi,
  klip animasi — semuanya sudah dipakai nyata oleh penulis konten (E7).
- Antarmuka `IViewportRenderer` yang sudah dipanggil dari lima panel berbeda.

Artinya E8 adalah menulis implementasi kedua dari satu antarmuka yang sudah mapan.

## E8 — Renderer

### E8.1 — Render graph & pass dasar
Frame graph sederhana (deklarasi pass + resource, penyusunan barrier otomatis),
depth prepass, forward opaque, transparan tersortir, resolve. Kamera & frustum
culling. Bindless descriptor untuk tekstur (`VK_EXT_descriptor_indexing`).

### E8.2 — Material runtime
Kompilasi graph `.simmat` → kode shader → SPIR-V, cache di disk berbasis hash graph.
Material instance jadi uniform buffer + indeks tekstur. Varian shader (skinned,
instanced, alpha-test) lewat spesialisasi konstanta. **Titik sambung:** Material
Editor mulai menampilkan preview PBR sungguhan.

### E8.3 — Lighting & shadow
Model shading **OpenPBR Surface v1.1** (`openpbr.slang`), IBL (prefilter env +
DFG LUT), directional light dengan cascaded shadow map, point/spot dengan shadow
atlas, clustered light culling untuk banyak lampu.

`evalOpenPBR_IBL` menerima `prefilteredBase`, `prefilteredCoat`, dan
`irradiance` terpisah. Ketiganya di sini datang dari probe statis — dan itu juga
titik sambung untuk global illumination, yang mengganti sumbernya tanpa mengubah
satu baris pun model shading-nya. Lihat catatan GI di bawah.

### E8.4 — Mesh & animasi
Impor mesh (ufbx/cgltf) menggantikan importer pass-through E5, LOD dari
meshoptimizer, GPU skinning dengan skinning buffer, blend shape.
**Titik sambung:** Animation Editor memutar mesh skinned sungguhan.

### E8.5 — Terrain
Rendering terrain ter-tile dengan LOD berbasis jarak (clipmap atau quadtree),
sampling splat map, blending material layer, culling per-tile, hole.
**Titik sambung:** Terrain Editor melukis pada permukaan yang di-shading penuh.

### E8.6 — Vegetation
Instanced rendering dengan GPU culling (compute + indirect draw), LOD & transisi
billboard, animasi angin di vertex shader, impostor untuk jarak jauh.
**Titik sambung:** Vegetation Editor menampilkan sebaran sesungguhnya.

### E8.7 — Partikel
Simulasi GPU (compute shader) dengan modul yang sama seperti definisi editor,
sorting untuk transparansi, soft particle, ribbon/trail.
**Titik sambung:** Particle Editor memutar simulasi yang identik dengan runtime.

### E8.8 — Post-process & langit
Sky atmospheric, tone mapping (ACES), bloom, SSAO, TAA atau FXAA, depth of field,
motion blur, color grading LUT, exposure otomatis.

**Kriteria terima E8 (keseluruhan).** Scene uji berisi terrain 2×2 km, 200 ribu
instance vegetasi, 20 lampu berbayang, karakter ber-animasi, dan tiga sistem partikel
berjalan ≥ 60 fps pada GPU target, tanpa error validation layer, dan tampilan di
Editor identik dengan tampilan di SimRuntime.

## Global illumination

**Belum masuk daftar pass di atas, dan itu disengaja sampai ada keputusan.**
Rencana terpisah ada di `/home/arie/SDK/rencana-implementasi-gi.md`: screen probe
+ hash grid radiance cache di belakang satu antarmuka `ITraceBackend`, dengan dua
implementasi — SDF clipmap untuk GPU tanpa RT core, ray query untuk yang punya.
Anggarannya 3,0 ms per frame di 1080p, ±16 minggu untuk satu orang.

Tiga hal yang perlu diputuskan sebelum ia masuk roadmap:

- **Anggarannya belum didamaikan dengan kriteria terima E8.** 3,0 ms adalah 18%
  dari frame 60 fps, sementara adegan uji E8 — terrain 2×2 km, 200 ribu instance
  vegetasi, 20 lampu berbayang — sudah menuntut sisanya. Keduanya ditulis
  terpisah dan belum pernah dijumlahkan.
- **Ia bergantung pada E8.3, bukan hanya E8.1.** Milestone M6-nya menyambung ke
  `evalOpenPBR_IBL`, yang menuntut prefilter env dan DFG LUT sudah ada.
- **Baseline-nya tidak bisa diuji di mesin ini.** Mesin pengembangan punya RT
  core (RTX 2060), yaitu tier atas rencana itu. Jalur SDF — yang justru harus
  bekerja di semua GPU — hanya bisa diuji lewat override backend manual, jadi
  override itu bukan kemudahan melainkan syarat.

Kabar baiknya: **sisi shader M6 sudah selesai.** `evalOpenPBR_IBL` sudah
menerima `irradiance` terpisah, sudah mengalikannya dengan `(1 - E_spec)`, dan
sudah mengecualikan logam dari lobe difus lewat `lerp(..., metalness)`. Yang
tersisa hanya menyambungkan sumbernya.

## E9 — Runtime & distribusi

- **SimRuntime**: player tanpa editor yang memuat level + menjalankan Lua.
- **Cook/packaging**: konversi aset ke format biner siap-pakai, pak arsip,
  pemangkasan aset yang tidak terpakai lewat graf ketergantungan dari E5.
- **PhysX 5** (`/home/arie/SDK/PhysX-main`): rigid body, collider, character
  controller, raycast, dengan komponen dan visualisasi debug di editor.
- **Audio** (OpenAL Soft di `/home/arie/SDK/openal-soft-1.25.2`): sumber suara 3D,
  bus, mixing.
- **Play-in-Editor** yang sesungguhnya: menjalankan world sungguhan dalam proses
  editor, dengan pemisahan state agar Stop mengembalikan scene ke keadaan awal.
- Profiler (Tracy), build Windows, dan skrip rilis.

## Keputusan yang sudah dikunci sejak fase editor

Dicatat supaya tidak diperdebatkan ulang saat E8:

1. **Koordinat**: Y-up, tangan-kanan, satuan meter. Depth Vulkan `[0,1]` dengan
   reversed-Z (near = 1.0) untuk presisi jarak jauh — penting karena ada terrain.
2. **Warna**: seluruh perhitungan di ruang linear; sRGB hanya di titik input tekstur
   dan output akhir. Editor sudah menandai color space per tekstur sejak E5.
3. **Material**: **OpenPBR Surface v1.1**, bukan metallic-roughness sederhana
   maupun specular-glossiness. Node keluaran graph E7.1 sudah mencerminkan
   `OpenPBRSurface` pin per pin, dengan nilai bawaan yang dikunci test terhadap
   berkas normatif OpenPBR. `base_metalness` tetap ada di dalamnya — yang
   bertambah adalah lapis specular terpisah, coat, dan fuzz.

   Tiga kelompok parameter spesifikasi sengaja belum ada: `subsurface_*`,
   `transmission_*`, `thin_film_*`. Tekniknya sudah dipilih di
   [RENDER-OPENPBR.md](RENDER-OPENPBR.md).
4. **Shader**: sumbernya Slang, dikompilasi lewat `slangc` dari Vulkan SDK. GLSL
   masih diterima untuk shader utilitas.
5. **Vulkan 1.3** sebagai baseline (dynamic rendering, synchronization2,
   timeline semaphore), dengan fallback render pass tradisional bila perangkat
   hanya 1.2.
