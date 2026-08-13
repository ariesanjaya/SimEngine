# Rencana Implementasi Global Illumination

**Baseline:** GTX 1660 Super / RX 5600 XT (tanpa RT core, 6 GB)
**Tier atas:** RTX 2060 (30 RT core, 6 GB)
**Target:** 1080p 60 fps, anggaran GI ≤ 3,0 ms (baseline) / ≤ 3,5 ms (tier atas)

---

## Prinsip arsitektur

Satu keputusan yang menentukan seluruh rencana ini:

> **Backend tracing adalah plugin. Sisa sistem GI tidak boleh tahu ray-nya ditembak ke apa.**

```
interface ITraceBackend
{
    TraceResult trace(float3 origin, float3 dir, float tMax);
}
```

Implementasi:
- `SdfTraceBackend`   → screen-space HiZ + global SDF clipmap  (semua GPU)
- `RayQueryBackend`   → DXR 1.1 / VK_KHR_ray_query             (RTX 2060+)

Semua sistem di atasnya — screen probe, hash grid, denoiser, integrasi shading — identik di kedua tier. Kalau ada satu saja pass yang bercabang berdasarkan tier di luar backend ini, desainnya bocor dan biaya perawatannya akan berlipat.

---

## Anggaran

### Waktu per frame (1080p, 60 fps)

| Pass | Baseline | RTX 2060 |
|---|---|---|
| Update SDF clipmap / BVH refit | 0,4 ms | 0,3 ms |
| Screen probe trace | 1,4 ms | 1,2 ms |
| Hash grid update + resolve | 0,4 ms | 0,4 ms |
| Denoise (temporal + a-trous) | 0,6 ms | 0,6 ms |
| Upsample + integrate | 0,2 ms | 0,2 ms |
| **Total** | **3,0 ms** | **2,7 ms** + ray tambahan |

Sisa anggaran di RTX 2060 dipakai untuk menaikkan jumlah ray, bukan menambah pass baru.

### VRAM

| Alokasi | Baseline | RTX 2060 |
|---|---|---|
| Global SDF clipmap (3× 128³ R8) | 6 MB | 6 MB (tetap dipakai untuk fallback jauh) |
| Screen probe atlas + history | 20 MB | 20 MB |
| Hash grid radiance (2²⁰ × 16 B) | 16 MB | 16 MB |
| Radiance history ½ res RGBA16F | 8 MB | 8 MB |
| **BVH** | — | **150–400 MB** |
| **Total** | **~50 MB** | **~250–450 MB** |

⚠️ **BVH adalah risiko terbesar tier atas.** RTX 2060 juga hanya 6 GB. Rencanakan proxy-BVH (LOD kasar untuk objek jauh) sejak awal, bukan sebagai optimasi belakangan.

---

## Milestone

Tiap milestone punya kriteria selesai yang bisa diuji. Jangan lanjut sebelum kriterianya lulus — GI adalah sistem yang error-nya menumpuk diam-diam.

### M0 — Fondasi (±1 minggu)
- Interface `ITraceBackend` + stub yang selalu mengembalikan miss
- Debug view: albedo, normal, irradiance mentah, ray count, jumlah langkah march
- Counter profiling terpisah per pass (bukan satu angka "GI")

**Selesai kalau:** bisa menampilkan heatmap jumlah langkah SDF per pixel. Ini alat diagnostik yang paling sering kamu pakai selama 3 bulan ke depan.

### M1 — Global SDF clipmap (±3 minggu)
- ✅ Bake SDF per-mesh offline lewat OpenVDB → `sim::SdfGrid` padat per mesh, dikomposit lewat `BakedSceneField`. Opsional: tanpa OpenVDB, clipmap mundur ke `BoxSceneField`. Lihat [DEPENDENCIES.md](DEPENDENCIES.md)
- 3 kaskade 128³ R8_UNORM, voxel 10 cm / 40 cm / 1,6 m → jangkauan ±102 m
- Toroidal scroll: hanya irisan tepi yang ditulis ulang saat kamera bergerak
- Komposit objek statis + dinamis ke kaskade

**Selesai kalau:** sphere tracing dari kamera menghasilkan depth yang cocok dengan raster depth buffer (uji visual side-by-side), dan biaya update tetap < 0,5 ms saat kamera bergerak cepat di adegan uji terpadat.

### M2 — Lapis screen-space (±1 minggu)
- HiZ march di depth buffer, maks 16 langkah
- Fallback berjenjang: HiZ miss → global SDF → sky cubemap

**Selesai kalau:** `SdfTraceBackend` lengkap dan lulus uji ray tunggal terhadap referensi CPU.

### M3 — Screen probe (±3 minggu)
- Tile 16×16 px, oktahedral 4×4 = 16 ray/probe/frame
- Placement: snap ke permukaan via G-buffer depth, dengan probe tambahan untuk pixel disocclusion
- Jitter arah per frame (sekuens low-discrepancy), akumulasi 8–16 frame
- Interpolasi ke pixel dengan bobot depth + normal

**Selesai kalau:** Cornell box menunjukkan color bleeding yang benar dan stabil (tidak berdenyut) saat kamera diam.

### M4 — Hash grid radiance cache (±2 minggu)
- Kunci: posisi terkuantisasi + arah mayor, 2²⁰ entri fixed
- Diisi dari hit ray screen probe, di-resolve tiap frame
- Query saat ray mengenai permukaan → memberi **multi-bounce** hampir gratis

**Selesai kalau:** furnace test — adegan albedo 1,0 di bawah pencahayaan seragam mendekati putih rata, tidak menggelap.

### M5 — Denoise & temporal (±3 minggu)
- Reprojection dengan motion vector, tolak sampel via depth + normal
- A-trous 2 pass, bobot bilateral
- Penanganan disocclusion: naikkan ray count sementara di region baru

**Selesai kalau:** lampu dinyalakan-matikan, GI merespons < 200 ms tanpa ghosting yang terlihat.

### M6 — Integrasi ke shading OpenPBR (±1 minggu)
- Irradiance GI masuk sebagai `irradiance` di `evalOpenPBR_IBL`
- Hormati albedo scaling: kontribusi difus dikalikan `(1 - E_spec)`
- Metal mengambil dari lapis spekular, bukan irradiance difus

**Selesai kalau:** white furnace test lulus untuk seluruh rentang roughness dan metalness.

### M7 — Backend ray query (±2 minggu)
- `RayQueryBackend` di belakang interface yang sama
- Proxy-BVH untuk objek jauh; BVH refit (bukan rebuild) untuk objek dinamis
- Deteksi kapabilitas runtime → pilih backend otomatis, dengan override manual

**Selesai kalau:** kedua backend menghasilkan gambar yang secara perseptual setara di adegan uji yang sama; selisihnya hanya detail kontak dan geometri tipis.

### M8 — Tier & preset kualitas (±1 minggu)

| Parameter | Low | Medium | High (2060) |
|---|---|---|---|
| Backend | SDF | SDF | Ray query |
| Ray/probe/frame | 8 | 16 | 32 |
| Resolusi GI | ¼ | ½ | ½ |
| Kaskade SDF | 2 | 3 | 3 (fallback) |
| Langkah march maks | 24 | 32 | — |

---

## Yang sengaja TIDAK dibangun

Menahan diri di sini sama pentingnya dengan mengerjakan daftar di atas:

- **Mesh SDF per-objek sebagai lapis trace terpisah** — memori dan kompleksitasnya tidak sepadan di 6 GB
- **BVH software di compute shader** — 5–10× lebih lambat dari sphere tracing di GPU tanpa RT core
- **Denoiser berbasis ML** — TU116 tidak punya tensor core, RDNA1 apalagi
- **Specular GI penuh di baseline** — mulai dari SSR + probe fallback saja
- **GI volumetrik / participating media** — tunda sampai difus benar-benar stabil

---

## Risiko utama

| Risiko | Gejala | Mitigasi |
|---|---|---|
| Update SDF jadi pos biaya terbesar | Frame time melonjak saat banyak objek bergerak | Counter terpisah sejak M0; batasi objek dinamis yang masuk komposit |
| Geometri tipis hilang di SDF | Pagar/daun tidak menghasilkan bayangan indirect | Terima di baseline; ray query menutupinya di tier atas |
| BVH tidak muat di 6 GB | OOM di adegan besar pada RTX 2060 | Proxy-BVH + streaming, direncanakan di M7 bukan sesudahnya |
| Leaking lewat dinding tipis | Cahaya menembus ruangan | Naikkan resolusi kaskade terdekat; uji dengan adegan dinding 10 cm |
| Latensi temporal terlihat | Perubahan cahaya lambat menyusul | Naikkan ray count adaptif di region yang berubah |

---

## Estimasi total

**±16 minggu** untuk satu orang penuh waktu sampai M8. Angka ini optimis kalau ini implementasi GI pertamamu — M3 dan M5 (probe dan denoise) hampir selalu makan waktu 2× perkiraan awal, karena keduanya bukan soal algoritma melainkan soal menjinakkan artefak.

Urutan ini disusun supaya kamu punya GI yang **terlihat** sejak akhir M3, bukan di akhir semuanya. Itu penting untuk menjaga momentum dan untuk menemukan masalah kualitas selagi masih murah untuk diubah.

---

## Adegan uji yang perlu disiapkan sejak M0

1. **Cornell box** — validasi color bleeding & energi
2. **Furnace test** — albedo 1,0, pencahayaan seragam; harus hilang ke latar
3. **Dinding tipis 10 cm** — deteksi leaking
4. **Koridor dengan pintu** — occlusion jarak menengah
5. **Adegan padat + objek bergerak** — biaya update SDF/BVH
6. **Outdoor luas** — jangkauan kaskade & fallback langit
