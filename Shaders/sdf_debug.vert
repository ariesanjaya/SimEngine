#version 450

// Debug view SDF, bagian vertex: satu segitiga penutup layar dan sinar kamera
// per piksel. Pola yang sama dengan grid.vert — tidak ada vertex buffer, dan
// sinar dihitung dari matriks yang dibalik.

layout(push_constant) uniform Push {
    mat4 invViewProj;
    vec4 cameraPos;  // xyz posisi kamera, w depth bidang dekat
    vec4 params;     // x jenis debug view, y jangkauan trace maksimum
} pc;

layout(location = 0) out vec3 vNearPoint;
layout(location = 1) out vec3 vFarPoint;

vec3 Unproject(vec2 ndc, float depth) {
    vec4 point = pc.invViewProj * vec4(ndc, depth, 1.0);
    return point.xyz / point.w;
}

void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vec2 ndc = uv * 2.0 - 1.0;
    float nearDepth = pc.cameraPos.w;
    vNearPoint = Unproject(ndc, nearDepth);
    vFarPoint = Unproject(ndc, 1.0 - nearDepth);
    gl_Position = vec4(ndc, 0.0, 1.0);
}
