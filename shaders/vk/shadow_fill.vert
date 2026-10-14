#version 450
// Full-screen triangle for the shadow-volume fill (D3D draws a viewport quad, Scene::draw_shadow 0x5ee1d0).
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
