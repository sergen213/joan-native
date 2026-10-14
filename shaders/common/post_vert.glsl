// Full-screen triangle; v_uv follows the API's own image origin (GL bottom-left, Vulkan top-left).
VARY(0) out vec2 v_uv;
void main() {
    vec2 p = vec2((VERTEX_ID << 1) & 2, VERTEX_ID & 2);
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
