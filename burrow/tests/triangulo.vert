#version 450
// Burrow's drawing test: a triangle that covers the middle of the image, without vertex buffers
void main() {
    vec2 p[3] = vec2[](vec2(-0.8, 0.8), vec2(0.8, 0.8), vec2(0.0, -0.8));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
