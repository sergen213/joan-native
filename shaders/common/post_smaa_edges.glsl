// SMAA pass 1: luma edge detection of u_ldr into an RG target cleared to 0 (pixels without edges discard).
TEX(8) sampler2D u_ldr;
VARY(0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 offset[3];
    SMAAEdgeDetectionVS(v_uv, offset);
    o_color = vec4(SMAALumaEdgeDetectionPS(v_uv, offset, u_ldr), 0.0, 0.0);
}
