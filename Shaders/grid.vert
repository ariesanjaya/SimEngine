#version 450

// Grid tanah prosedural, bagian vertex.
//
// Tidak ada vertex buffer: tiga vertex membentuk segitiga yang menutupi seluruh
// layar, lalu fragment shader menghitung perpotongan sinar kamera dengan bidang
// y = 0. Keuntungannya, grid otomatis tak terbatas dan selalu tajam pada jarak
// berapa pun, tanpa perlu membangun ulang geometri saat kamera bergerak.

layout(push_constant) uniform Push {
    mat4 invViewProj;
    vec4 cameraPos;   // xyz = posisi kamera
    vec4 params;      // x = ukuran petak, y = jarak pudar, z = tebal sumbu,
                      // w = nilai depth bidang dekat (0 biasa, 1 reversed-Z)
} pc;

layout(location = 0) out vec3 vNearPoint;
layout(location = 1) out vec3 vFarPoint;

vec3 Unproject(vec2 ndc, float depth) {
    vec4 point = pc.invViewProj * vec4(ndc, depth, 1.0);
    return point.xyz / point.w;
}

void main() {
    // (0,0), (2,0), (0,2) -> NDC (-1,-1), (3,-1), (-1,3)
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vec2 ndc = uv * 2.0 - 1.0;

    // Vulkan memakai rentang depth [0,1]. Bidang dekat ada di 0 pada proyeksi
    // biasa dan di 1 pada reversed-Z — jadi pemanggil yang menyebutkannya, bukan
    // shader yang menebak. Menebaknya berarti grid yang menghilang begitu
    // renderer berganti proyeksi, tanpa satu pun pesan galat.
    float nearDepth = pc.params.w;
    vNearPoint = Unproject(ndc, nearDepth);
    vFarPoint = Unproject(ndc, 1.0 - nearDepth);

    gl_Position = vec4(ndc, 0.0, 1.0);
}
