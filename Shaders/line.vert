#version 450

// Garis ruang-dunia untuk wireframe seleksi, sumbu, dan ikon billboard.
// Semuanya sudah dibentangkan jadi segmen di CPU oleh renderer — termasuk
// billboard, yang butuh posisi kamera untuk menghadap dan ukuran layar untuk
// menjaga besarnya tetap dalam piksel.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;

layout(push_constant) uniform Push {
    mat4 viewProj;
} pc;

layout(location = 0) out vec4 vColor;

void main() {
    gl_Position = pc.viewProj * vec4(inPosition, 1.0);
    vColor = inColor;
}
