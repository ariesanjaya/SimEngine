# Rencana Implementasi: Software Ray Traced GI (Embree Builder + Vulkan Compute Traversal)

> Dokumen ini ditulis untuk dieksekusi langsung oleh Claude Code. Setiap milestone punya
> daftar file yang harus dibuat, kontrak data, dan *Definition of Done* yang bisa diverifikasi.
> Kerjakan milestone secara berurutan. Jangan lompat ke M5 sebelum M4 lulus validasi.

---

## 0. Keputusan Arsitektur (baca dulu, jangan diubah tanpa alasan)

**Embree dipakai HANYA sebagai BVH builder di CPU, bukan sebagai tracer runtime.**

Alasan:
- Embree tidak punya interop apa pun dengan Vulkan. `RTCScene` tidak bisa jadi `VkAccelerationStructureKHR`.
- Tracing di CPU untuk GI real-time kehabisan budget: 1080p @60fps dengan 1 ray/piksel butuh
  ~124 Mrays/s, sementara Embree di CPU desktop 8-core hanya puluhan Mrays/s untuk ray inkoheren.
- Target hardware minimum (GTX 1660 Super) punya compute menganggur. Traversal BVH software di
  compute shader mengungguli CPU untuk ray inkoheren, tanpa PCIe round-trip dan tanpa sinkronisasi.

**Yang kita ambil dari Embree**: kualitas builder-nya (SAH + spatial split / SBVH) tanpa harus
menulis SBVH builder sendiri. Builder jalan saat load time / offline bake, hasilnya di-flatten ke
layout GPU dan diupload sekali.

**Tiering** (sesuai target hardware bertingkat):

| Tier | Hardware | Backend trace |
|------|----------|---------------|
| `SW`  | GTX 1660 Super ke atas | Compute shader traversal BVH4 (fokus dokumen ini) |
| `HW`  | RTX 2060 ke atas | `VK_KHR_ray_query`, shading code identik |

Kedua tier berbagi ray generation, shading, dan integrasi GI yang sama. Hanya fungsi
`traceRay()` yang berbeda implementasinya.

**Algoritma GI v1**: DDGI (irradiance probe volume, oktahedral) — dipilih karena ray budget-nya
terkontrol dan tidak sensitif terhadap latency. Surfel caching bisa menyusul di fase berikutnya
memakai infrastruktur trace yang sama.

---

## 1. Prasyarat & Dependensi

```
Embree      >= 4.3   (Apache-2.0)      — builder saja, tidak dipakai runtime
Vulkan SDK  >= 1.3.268
Slang       >= 2024.x                  — semua shader ditulis di Slang, compile ke SPIR-V
VMA         >= 3.1                     — alokasi buffer
CMake       >= 3.24
```

Embree di-fetch via `FetchContent` dengan opsi minimal — kita **tidak** butuh device runtime-nya:

```cmake
set(EMBREE_ISPC_SUPPORT      OFF CACHE BOOL "" FORCE)
set(EMBREE_TUTORIALS         OFF CACHE BOOL "" FORCE)
set(EMBREE_STATIC_LIB        ON  CACHE BOOL "" FORCE)
set(EMBREE_TASKING_SYSTEM    "INTERNAL" CACHE STRING "" FORCE)  # hindari dependensi TBB
set(EMBREE_GEOMETRY_QUAD     OFF CACHE BOOL "" FORCE)
set(EMBREE_GEOMETRY_CURVE    OFF CACHE BOOL "" FORCE)
set(EMBREE_GEOMETRY_SUBDIVISION OFF CACHE BOOL "" FORCE)
set(EMBREE_GEOMETRY_POINT    OFF CACHE BOOL "" FORCE)
```

> Catatan lisensi: Embree Apache-2.0, aman untuk produk komersial tertutup. Dengan
> `EMBREE_TASKING_SYSTEM=INTERNAL` tidak ada dependensi TBB sama sekali.

---

## 2. Struktur Direktori Target

```
engine/
  gi/
    bvh/
      bvh_types.h            # struct shared CPU<->GPU
      bvh_builder.h/.cpp     # wrapper rtcBuildBVH
      bvh_flatten.h/.cpp     # BVH intermediate -> layout GPU
      bvh_validate.h/.cpp    # sanity check + reference compare
      bvh_gpu.h/.cpp         # upload, descriptor, lifetime
    ddgi/
      ddgi_volume.h/.cpp     # konfigurasi + resource probe
      ddgi_pass.h/.cpp       # orkestrasi pass per frame
    backend/
      rt_backend.h           # interface abstrak
      rt_backend_sw.cpp      # jalur compute traversal
      rt_backend_hw.cpp      # jalur VK_KHR_ray_query
  shaders/slang/gi/
    bvh_traverse.slang       # fungsi traceRay() software
    rt_interface.slang       # deklarasi umum Ray/Hit + switch backend
    ddgi_raygen.slang
    ddgi_shade.slang
    ddgi_blend_irradiance.slang
    ddgi_blend_depth.slang
    ddgi_sample.slang        # dipakai lighting pass
tools/
  bvh_bake/                  # CLI opsional: bake BVH ke file
tests/
  test_bvh_reference.cpp     # GPU vs Embree runtime, ray-by-ray
```

---

## 3. Kontrak Data (definisikan lebih dulu, semua milestone bergantung ke sini)

`engine/gi/bvh/bvh_types.h` — layout harus identik di C++ dan Slang (std430).

```cpp
// 128 byte, satu node BVH4. Layout SoA-per-node agar bisa di-load 4-wide di GPU.
struct GpuBvh4Node {
    float minx[4], miny[4], minz[4];   // 48 B
    float maxx[4], maxy[4], maxz[4];   // 48 B
    uint32_t child[4];                 // 16 B
    uint32_t _pad[4];                  // 16 B -> total 128 B
};
// Encoding child[i]:
//   0xFFFFFFFF          = slot kosong
//   MSB = 0             = index node internal (offset dalam array node)
//   MSB = 1             = leaf; bit [30:24] = primCount (1..64),
//                                bit [23:0]  = primOffset (index ke array primIndex)

// 48 byte per triangle. w-component dipakai untuk id agar hemat fetch.
struct GpuTriangle {
    float v0[3]; uint32_t primID;
    float v1[3]; uint32_t geomID;
    float v2[3]; uint32_t materialID;
};

struct GpuInstance {          // TLAS
    float worldToObject[12];  // affine 3x4, row-major
    float objectToWorld[12];
    uint32_t blasNodeOffset;
    uint32_t blasTriOffset;
    uint32_t geomID;
    uint32_t _pad;
};
```

Binding set GI (set = 1):

| binding | tipe | isi |
|---------|------|-----|
| 0 | SSBO ro | `GpuBvh4Node nodes[]` |
| 1 | SSBO ro | `GpuTriangle tris[]` |
| 2 | SSBO ro | `uint primIndex[]` |
| 3 | SSBO ro | `GpuInstance instances[]` |
| 4 | SSBO ro | `GpuMaterialLite materials[]` (albedo + emissive, tanpa tekstur) |
| 5 | image rw | irradiance atlas |
| 6 | image rw | depth/visibility atlas |
| 7 | UBO | `DdgiVolumeConstants` |

---

## 4. Milestone

### M0 — Skeleton & build system
**Buat**: `CMakeLists.txt` di `engine/gi/`, target `engine_gi`, FetchContent Embree, integrasi
`slangc` sebagai custom command (compile semua `.slang` di `shaders/slang/gi/` ke `.spv`).

**DoD**: `cmake --build .` sukses di Linux + Windows; `engine_gi` link ke `embree4` static;
satu shader dummy tercompile ke SPIR-V dan tervalidasi dengan `spirv-val`.

---

### M1 — Ekstraksi geometri CPU-side
Kumpulkan geometri statik scene jadi buffer flat yang jadi input builder.

**Buat**: `bvh_builder.h` dengan struct input:

```cpp
struct BvhBuildInput {
    const float*    vertices;     // xyz, stride bebas
    size_t          vertexStride;
    size_t          vertexCount;
    const uint32_t* indices;      // triangle list
    size_t          triangleCount;
    uint32_t        geomID;
};
```

**Aturan**: mesh yang alpha-tested/foliage **dikecualikan** di v1 (Embree bisa handle lewat filter
callback tapi mahal di GPU traversal). Tandai dengan flag material dan skip.

**DoD**: fungsi `collectStaticGeometry(Scene&) -> std::vector<BvhBuildInput>` mengembalikan jumlah
triangle yang cocok dengan hitungan scene; unit test dengan scene 2 kubus.

---

### M2 — Build BVH dengan `rtcBuildBVH`

Ini inti pemakaian Embree. Kita pakai API builder standalone, **bukan** `rtcNewScene`.

**Buat**: `bvh_builder.cpp`.

```cpp
#include <embree4/rtcore.h>

struct BuildCtx {
    std::vector<BuildNode> nodes;   // intermediate, pointer-based
    std::vector<BuildLeaf> leaves;
    std::mutex             mtx;     // callback dipanggil multi-thread
};

static void* createNode(RTCThreadLocalAllocator alloc, unsigned childCount, void* userPtr) {
    void* p = rtcThreadLocalAlloc(alloc, sizeof(BuildNode), 16);
    return new (p) BuildNode(childCount);
}

static void setNodeChildren(void* nodePtr, void** children, unsigned n, void* userPtr) {
    auto* node = static_cast<BuildNode*>(nodePtr);
    for (unsigned i = 0; i < n; ++i) node->child[i] = children[i];
    node->childCount = n;
}

static void setNodeBounds(void* nodePtr, const RTCBounds** bounds, unsigned n, void* userPtr) {
    auto* node = static_cast<BuildNode*>(nodePtr);
    for (unsigned i = 0; i < n; ++i) node->bounds[i] = *bounds[i];
}

static void* createLeaf(RTCThreadLocalAllocator alloc,
                        const RTCBuildPrimitive* prims, size_t primCount, void* userPtr) {
    void* p = rtcThreadLocalAlloc(alloc, sizeof(BuildLeaf), 16);
    return new (p) BuildLeaf(prims, primCount);   // simpan geomID/primID
}

// Wajib ada kalau buildQuality = HIGH (spatial split).
static void splitPrimitive(const RTCBuildPrimitive* prim, unsigned dim, float pos,
                           RTCBounds* lbounds, RTCBounds* rbounds, void* userPtr) {
    *lbounds = *rbounds = toBounds(*prim);
    (&lbounds->upper_x)[dim] = pos;
    (&rbounds->lower_x)[dim] = pos;
}
```

Pemanggilan:

```cpp
RTCDevice device = rtcNewDevice("threads=0");
RTCBVH    bvh    = rtcNewBVH(device);

// HIGH quality memakai spatial split -> butuh kapasitas array ekstra.
const size_t capacity = size_t(primCount * 1.2);
std::vector<RTCBuildPrimitive> prims(capacity);   // isi 0..primCount-1
// RTCBuildPrimitive: {lower_x,lower_y,lower_z, geomID, upper_x,upper_y,upper_z, primID}

RTCBuildArguments args = rtcDefaultBuildArguments();
args.byteSize               = sizeof(args);
args.buildQuality           = RTC_BUILD_QUALITY_HIGH;
args.maxBranchingFactor     = 4;          // langsung BVH4, hindari collapse manual
args.maxDepth               = 64;         // batasi -> stack GPU terkontrol
args.sahBlockSize           = 1;
args.minLeafSize            = 1;
args.maxLeafSize            = 8;
args.traversalCost          = 1.0f;
args.intersectionCost       = 1.0f;
args.bvh                    = bvh;
args.primitives             = prims.data();
args.primitiveCount         = primCount;
args.primitiveArrayCapacity = capacity;
args.createNode             = createNode;
args.setNodeChildren        = setNodeChildren;
args.setNodeBounds          = setNodeBounds;
args.createLeaf             = createLeaf;
args.splitPrimitive         = splitPrimitive;
args.userPtr                = &ctx;

void* root = rtcBuildBVH(&args);
// ... flatten dari `root` (M3) SEBELUM rtcReleaseBVH, memori node milik BVH.
rtcReleaseBVH(bvh);
rtcReleaseDevice(device);
```

**Peringatan implementasi**:
- Callback dipanggil dari banyak thread. `rtcThreadLocalAlloc` sudah thread-safe; jangan alokasi
  ke `std::vector` bersama tanpa lock.
- Memori node hidup selama `RTCBVH` hidup. Flatten dulu, baru release.
- `maxDepth = 64` bukan kosmetik: itu yang menentukan ukuran stack traversal di GPU.
- Kalau `maxBranchingFactor = 4` bermasalah dengan `RTC_BUILD_QUALITY_HIGH` di versi Embree yang
  dipakai, fallback: build dengan factor 2 lalu collapse BVH2→BVH4 di M3 (kumpulkan cucu dengan
  SAH terbesar). Verifikasi ke dokumentasi Embree versi terpasang, jangan asumsi.

**DoD**: build selesai tanpa crash untuk scene ≥500k triangle; log kedalaman maksimum, jumlah node,
jumlah leaf, rata-rata prim per leaf (target 2–4).

---

### M3 — Flatten ke layout GPU

**Buat**: `bvh_flatten.cpp`. Traversal DFS dari `root`, tulis `GpuBvh4Node` berurutan.

Aturan:
- Anak ditulis berdekatan (child locality) untuk cache behavior.
- Slot kosong diisi AABB degenerate (`min = +INF`, `max = -INF`) supaya slab test selalu miss —
  ini menghindari branch di shader.
- Leaf: append `primIndex` ke array global, encode offset+count ke `child[i]`.
- Bangun juga array `GpuTriangle` sesuai urutan `primIndex` untuk locality.

**DoD**: `bvh_validate.cpp` menjalankan cek: (a) semua index dalam range, (b) AABB parent
menyelubungi semua anak, (c) setiap triangle muncul minimal sekali di leaf, (d) tidak ada siklus.
Test lulus untuk 3 scene berbeda.

---

### M4 — Upload ke Vulkan

**Buat**: `bvh_gpu.cpp`. Alokasi VMA `DEVICE_LOCAL`, upload via staging buffer + transfer queue,
descriptor set layout sesuai tabel di §3.

**DoD**: buffer terupload, `vkCmdCopyBuffer` selesai dengan fence, readback 64 byte pertama cocok
dengan data CPU.

---

### M5 — Compute traversal kernel (Slang)

**Buat**: `shaders/slang/gi/bvh_traverse.slang`.

Desain:
- **Stack-based**, stack di array lokal `uint stack[32]` (bukan shared memory — occupancy lebih baik
  di Turing untuk workload ini). Depth build dibatasi 64 tapi ordered traversal dengan early-out
  jarang melebihi 32; tambahkan guard.
- **Ordered traversal**: hitung `tmin` per 4 anak, urutkan 4 elemen dengan sorting network
  (5 compare-exchange), push dari jauh ke dekat.
- **Slab test** 4-wide manual (bukan loop) supaya compiler bisa vektorkan.
- **Triangle test**: Möller–Trumbore di v1. Ganti ke watertight (Woop) hanya kalau muncul crack
  artifact di scene besar.
- Dua entry point: `traceClosest(Ray) -> Hit` dan `traceAnyHit(Ray) -> bool` (untuk shadow ray,
  early exit di hit pertama — ini yang paling sering dipakai, optimalkan duluan).

```hlsl
struct Ray  { float3 o; float tmin; float3 d; float tmax; };
struct Hit  { float t; float2 bary; uint primIndex; uint instanceIndex; };

Hit traceClosest(Ray ray) {
    float3 invD = 1.0 / ray.d;      // hati-hati inf; pakai safe reciprocal
    uint stack[32]; int sp = 0;
    uint nodeIdx = 0;
    Hit hit; hit.t = ray.tmax; hit.primIndex = ~0u;
    // ... loop: internal node -> slab test 4-wide -> sort -> push
    //           leaf         -> loop triangle -> Möller–Trumbore -> update hit.t
    return hit;
}
```

**Peringatan**: `1.0/0.0` di GLSL/SPIR-V menghasilkan inf yang benar untuk slab test, tapi
`0 * inf = NaN` merusak perbandingan. Pakai pola standar: clamp komponen arah yang nol ke `1e-8`
dengan mempertahankan tanda.

**DoD**: `tests/test_bvh_reference.cpp` membangkitkan 1 juta ray acak (campuran koheren dan
inkoheren), menembakkannya ke **Embree runtime** (`rtcNewScene` + `rtcIntersect1`) sebagai
ground truth dan ke kernel GPU, lalu membandingkan `primID` dan `t` (toleransi relatif 1e-4).
**Target: ≥99.99% cocok.** Ini gate paling penting di seluruh proyek — jangan lanjut kalau gagal.

> Di sinilah Embree runtime tetap berguna meski tidak dipakai di produk akhir: sebagai oracle
> kebenaran. Kompilasi jalur ini hanya di build test.

---

### M6 — TLAS & objek dinamis

- BLAS per-mesh dibangun sekali (hasil M2–M3), di-cache ke disk (`tools/bvh_bake`).
- TLAS berisi instance, dibangun ulang **tiap frame di CPU** dengan `rtcBuildBVH`
  `RTC_BUILD_QUALITY_LOW` atas AABB instance. Jumlah instance kecil (ratusan–ribuan), jadi ini
  submilidetik.
- Traversal: ray ditransformasi ke object space pakai `worldToObject`, lanjut ke BLAS.

**DoD**: 200 objek bergerak, TLAS rebuild < 0.5 ms di CPU, tidak ada ghosting pada hasil trace.

---

### M7 — DDGI di atas infrastruktur trace

**Konfigurasi v1** (angka ini sudah dihitung agar muat di budget 1660 Super):

```
Volume        : 24 x 12 x 24 = 6912 probe, spacing 1.5 m
Ray per probe : 128
Update        : 1/4 probe per frame (round-robin)  -> ~221k ray/frame
Irradiance    : 8x8 oktahedral + 1px border  -> atlas (24*24) x 12 tile @10x10, RGBA16F
Depth/vis     : 16x16 oktahedral + 1px border -> RG16F (mean, mean^2)
Hysteresis    : 0.97 irradiance, 0.97 depth
```

Estimasi: 221k ray/frame @ ~200 Mrays/s → ~1.1 ms. Sisakan budget untuk shading hit.

**Pass per frame** (semua compute):
1. `ddgi_raygen` — bangkitkan arah dengan spherical Fibonacci + rotasi acak per frame (quaternion
   random per update) untuk menghindari banding. Tulis ke ray buffer.
2. `ddgi_shade` — trace `traceClosest`, lalu shade hit:
   - albedo dari `GpuMaterialLite` (albedo konstan per material di v1, **tanpa sampling tekstur**),
   - direct light: 1 shadow ray `traceAnyHit` ke sun,
   - multi-bounce: sample irradiance probe **frame sebelumnya** di titik hit (feedback loop —
     ini yang memberi bounce tak hingga dengan biaya nol),
   - miss: sample sky/atmosphere LUT (nyambung ke sistem sky yang sudah ada).
3. `ddgi_blend_irradiance` — 1 workgroup per probe, akumulasi cosine-weighted ke texel oktahedral,
   blend dengan hysteresis, copy border.
4. `ddgi_blend_depth` — sama tapi untuk (dist, dist²).
5. `ddgi_sample.slang` — dipakai deferred lighting pass: trilinear antar 8 probe terdekat dengan
   bobot Chebyshev (variance shadow) + normal bias + wrap-around weight, untuk menekan light leak.

**DoD**: Cornell box menunjukkan color bleeding yang benar; scene indoor tidak bocor cahaya menembus
dinding tipis; frame time GI total < 2.0 ms di 1660 Super @1080p.

---

### M8 — Backend hardware (RTX 2060+)

**Buat**: `rt_backend_hw.cpp` + varian `rt_interface.slang` dengan `#define RT_BACKEND_HW`.

Ganti isi `traceClosest`/`traceAnyHit` dengan `RayQuery` (`VK_KHR_ray_query`), pakai
`VkAccelerationStructureKHR` yang dibangun via `vkCmdBuildAccelerationStructuresKHR`. Semua pass
DDGI **tidak berubah sedikit pun**. Naikkan ray per probe ke 256 dan update 1/2 probe per frame
untuk tier ini.

**DoD**: hasil visual kedua backend berselisih < 2% (bandingkan irradiance atlas secara numerik);
runtime memilih backend dari `VkPhysicalDeviceRayQueryFeaturesKHR`.

---

### M9 — Tooling & profiling

- Debug view: visualisasi probe sebagai bola (irradiance / depth / hysteresis state), heatmap
  jumlah node yang dikunjungi per ray (deteksi BVH buruk), overlay ray count.
- Timestamp query per pass, tulis ke CSV.
- CLI `bvh_bake`: glTF → file `.bvh` (mmap-able, header + versi + checksum).

---

## 5. Urutan Eksekusi yang Disarankan untuk Claude Code

```
M0 -> M1 -> M2 -> M3 -> M4 -> M5 (+ test_bvh_reference WAJIB HIJAU) -> M7 -> M6 -> M8 -> M9
```

M6 sengaja setelah M7: lebih mudah men-debug DDGI di scene statik dulu.

---

## 6. Risiko & Mitigasi

| Risiko | Mitigasi |
|--------|----------|
| Traversal GPU divergent, throughput jauh di bawah target | Ukur dulu dengan heatmap node-visited. Kalau leaf terlalu gemuk, turunkan `maxLeafSize` ke 4. Kalau masih rendah, coba BVH8 + node terkompresi (quantized AABB 8-bit) — memori turun 2x, bandwidth turun. |
| `maxBranchingFactor=4` + `QUALITY_HIGH` tidak didukung versi Embree terpasang | Fallback build BVH2 lalu collapse ke BVH4 di M3. Sudah disebut di M2. |
| Alpha-tested foliage hilang dari GI | v1 memang mengecualikannya. Fase berikut: tambah flag per-triangle + sampling alpha texture mip terkecil di leaf test. |
| Shading hit tanpa tekstur terlihat salah warna | Naikkan ke surface cache ala Lumen: bake albedo per-cluster resolusi rendah ke atlas, sample di shading hit. Ini fase terpisah, jangan dikerjakan di v1. |
| Light leak DDGI di geometri tipis | Sudah dimitigasi Chebyshev + normal bias. Kalau masih bocor, tambah cascade dengan spacing lebih rapat di dekat kamera. |
| Rebuild BVH untuk mesh terdeformasi (skinned) | Jangan masukkan skinned mesh ke BVH di v1. Fase berikut: BLAS refit (`QUALITY_REFIT`) per N frame, bukan rebuild. |

---

## 7. Yang TIDAK Dikerjakan di v1 (jangan scope creep)

- Surfel / radiance caching (menyusul, memakai `traceRay` yang sama)
- Reflection ray tracing
- Surface cache dengan tekstur
- Skinned mesh di dalam BVH
- Transparansi / refraksi
- Embree sebagai tracer runtime di CPU
