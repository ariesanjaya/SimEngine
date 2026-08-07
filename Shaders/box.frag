#version 450

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 outColor;

void main() {
    // Pencahayaan sementara: satu arah tetap plus ambient. Bukan model shading —
    // OpenPBR masuk di E8.2/E8.3, dan menulis pendekatan kedua di sini justru
    // melanggar aturan yang dipegang seluruh E7.1: model shading hanya boleh
    // punya satu implementasi. Ini sekadar supaya bentuk kotaknya terbaca.
    const vec3 lightDir = normalize(vec3(-0.4, 0.8, 0.45));
    float ndotl = max(dot(normalize(inNormal), lightDir), 0.0);
    vec3 lit = inColor.rgb * (0.25 + 0.75 * ndotl);
    outColor = vec4(lit, inColor.a);
}
