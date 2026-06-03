#version 450
// Fullscreen triangle, no vertex buffer needed. Draw(3, 1, 0, 0).
layout(location = 0) out vec2 vUV;
void main() {
    vec2 pos = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    vUV = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
