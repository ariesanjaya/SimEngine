// Atmosfer Bruneton-Hillaire, bagian yang dipakai bersama seluruh pass langit.
//
// **Angkanya harus sama persis dengan `Code/Render/src/Atmosphere.cpp`.** Yang
// di CPU diuji terhadap hitungan analitik; yang di sini yang benar-benar
// terlihat. Fisika yang berbeda antara keduanya membuat setiap uji lulus dan
// setiap langit salah.
//
// **Satuannya kilometer**, sama dengan acuannya: jari-jari bumi 6360 dan
// hamburan berorde 1e-3 per kilometer. Mencampurnya dengan meter — satuan sisa
// engine — tidak menghasilkan galat apa pun, hanya langit yang seluruhnya hitam
// atau seluruhnya putih, bergantung arah kesalahannya.

static const float kPi = 3.14159265358979323846;

/// Geseran dari permukaan planet sebelum integrasi, kilometer. Tanpa itu, sinar
/// yang berangkat tepat di permukaan bisa "mengenai" planet pada langkah nol
/// karena galat pembulatan — dan yang terlihat adalah cincin hitam di horizon.
static const float kRadiusOffset = 0.01;

static const float kBottomRadius = 6360.0;
static const float kTopRadius = 6460.0;
static const float3 kRayleighScattering = float3(0.005802, 0.013558, 0.033100);
static const float kRayleighScaleHeight = 8.0;
static const float kMieScattering = 0.003996;
static const float kMieExtinction = 0.004440;
static const float kMieScaleHeight = 1.2;
static const float kMiePhaseG = 0.8;
static const float3 kOzoneAbsorption = float3(0.000650, 0.001881, 0.000085);
static const float kOzoneBottom = 10.0;
static const float kOzonePeak = 25.0;
static const float kOzoneTop = 40.0;

float safeSqrt(float value) { return sqrt(max(value, 0.0)); }

float subUvToUnit(float u, float resolution) {
    return (u - 0.5 / resolution) * (resolution / (resolution - 1.0));
}

float unitToSubUv(float u, float resolution) {
    return (u + 0.5 / resolution) * (resolution / (resolution + 1.0));
}

/// Jarak ke perpotongan terdekat sebuah sinar dengan bola berpusat di titik
/// asal, atau −1 bila tidak ada di depan.
float raySphereNearest(float3 origin, float3 direction, float radius) {
    float a = dot(direction, direction);
    float b = 2.0 * dot(direction, origin);
    float c = dot(origin, origin) - radius * radius;
    float discriminant = b * b - 4.0 * a * c;
    if (discriminant < 0.0 || a == 0.0) { return -1.0; }
    float root = safeSqrt(discriminant);
    float near = (-b - root) / (2.0 * a);
    float far = (-b + root) / (2.0 * a);
    if (near < 0.0 && far < 0.0) { return -1.0; }
    if (near < 0.0) { return max(0.0, far); }
    if (far < 0.0) { return max(0.0, near); }
    return max(0.0, min(near, far));
}

struct Density {
    float rayleigh;
    float mie;
    float ozone;
};

Density sampleDensity(float heightKm) {
    Density density;
    density.rayleigh = exp(-heightKm / kRayleighScaleHeight);
    density.mie = exp(-heightKm / kMieScaleHeight);
    // Profil tenda, bukan eksponensial: ozon berkumpul di sebuah lapisan di
    // ketinggian, bukan meluruh dari permukaan. Tanpa serapannya, langit siang
    // hari condong ke kuning kehijauan alih-alih biru.
    float rising = (heightKm - kOzoneBottom) / (kOzonePeak - kOzoneBottom);
    float falling = (kOzoneTop - heightKm) / (kOzoneTop - kOzonePeak);
    density.ozone = saturate(heightKm < kOzonePeak ? rising : falling);
    return density;
}

float3 sampleExtinction(float heightKm) {
    Density density = sampleDensity(heightKm);
    return kRayleighScattering * density.rayleigh + kMieExtinction * density.mie +
           kOzoneAbsorption * density.ozone;
}

struct Scattering {
    float3 rayleigh;
    float3 mie;
};

Scattering sampleScattering(float heightKm) {
    Density density = sampleDensity(heightKm);
    Scattering scattering;
    scattering.rayleigh = kRayleighScattering * density.rayleigh;
    // Ozon tidak ikut: ia menyerap tanpa menghamburkan.
    scattering.mie = float3(kMieScattering * density.mie);
    return scattering;
}

float rayleighPhase(float cosTheta) {
    return 3.0 / (16.0 * kPi) * (1.0 + cosTheta * cosTheta);
}

float miePhase(float cosTheta, float g) {
    float g2 = g * g;
    float numerator = (1.0 - g2) * (1.0 + cosTheta * cosTheta);
    float denominator = (2.0 + g2) * pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-6), 1.5);
    return 3.0 / (8.0 * kPi) * numerator / denominator;
}

// --- Pemetaan LUT ------------------------------------------------------------
//
// **Tidak seragam, dan itu bukan hiasan.** Pemetaan seragam menghabiskan hampir
// seluruh texel pada sudut yang jarang dipandang dan menyisakan beberapa texel
// untuk pita tipis di dekat horizon — yaitu bagian langit yang paling
// diperhatikan orang.

/// uv → (jari-jari, kosinus sudut zenit).
float2 uvToTransmittanceParams(float2 uv) {
    float H = safeSqrt(kTopRadius * kTopRadius - kBottomRadius * kBottomRadius);
    float rho = H * uv.y;
    float radius = safeSqrt(rho * rho + kBottomRadius * kBottomRadius);
    float minDistance = kTopRadius - radius;
    float maxDistance = rho + H;
    float distance = minDistance + uv.x * (maxDistance - minDistance);
    float cosZenith = distance == 0.0
                          ? 1.0
                          : (H * H - rho * rho - distance * distance) / (2.0 * radius * distance);
    return float2(radius, clamp(cosZenith, -1.0, 1.0));
}

/// (jari-jari, kosinus sudut zenit) → uv.
float2 transmittanceParamsToUv(float2 params) {
    float H = safeSqrt(kTopRadius * kTopRadius - kBottomRadius * kBottomRadius);
    float rho = safeSqrt(params.x * params.x - kBottomRadius * kBottomRadius);
    float discriminant = params.x * params.x * (params.y * params.y - 1.0) + kTopRadius * kTopRadius;
    float distance = max(0.0, -params.x * params.y + safeSqrt(discriminant));
    float minDistance = kTopRadius - params.x;
    float maxDistance = rho + H;
    float mu = (distance - minDistance) / max(maxDistance - minDistance, 1e-6);
    return float2(mu, rho / max(H, 1e-6));
}

/// uv → (sudut zenit pandangan, sudut antara pandangan dan matahari).
float2 uvToSkyViewParams(float2 uv, float2 dimensions, float viewHeight) {
    float2 unit = float2(subUvToUnit(uv.x, dimensions.x), subUvToUnit(uv.y, dimensions.y));
    float beta = asin(clamp(kBottomRadius / viewHeight, -1.0, 1.0));
    float zenithHorizon = kPi - beta;

    float viewZenithAngle;
    if (unit.y < 0.5) {
        float coordinate = 1.0 - (1.0 - 2.0 * unit.y) * (1.0 - 2.0 * unit.y);
        viewZenithAngle = zenithHorizon * coordinate;
    } else {
        float coordinate = (unit.y * 2.0 - 1.0) * (unit.y * 2.0 - 1.0);
        viewZenithAngle = zenithHorizon + beta * coordinate;
    }
    return float2(viewZenithAngle, unit.x * unit.x * kPi);
}

float2 skyViewParamsToUv(float2 params, float2 dimensions, float viewHeight,
                         bool intersectsGround) {
    float beta = asin(clamp(kBottomRadius / viewHeight, -1.0, 1.0));
    float zenithHorizon = kPi - beta;

    float2 uv;
    if (!intersectsGround) {
        float coordinate = params.x / max(zenithHorizon, 1e-6);
        uv.y = (1.0 - safeSqrt(1.0 - coordinate)) / 2.0;
    } else {
        float coordinate = (params.x - zenithHorizon) / max(beta, 1e-6);
        uv.y = (safeSqrt(coordinate) + 1.0) / 2.0;
    }
    uv.x = safeSqrt(params.y / kPi);
    return float2(unitToSubUv(uv.x, dimensions.x), unitToSubUv(uv.y, dimensions.y));
}
