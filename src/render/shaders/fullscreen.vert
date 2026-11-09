#version 450
// 1.9.1: triangulo que cobre a viewport inteira (passagens de pos-processamento do port: FSR)
void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
