#version 450

// Satu segitiga penutup layar, tanpa vertex buffer dan tanpa varying. Yang
// dibutuhkan tahap fragment hanyalah `gl_FragCoord`, dan itu selalu ada.

void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
