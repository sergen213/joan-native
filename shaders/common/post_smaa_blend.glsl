// SMAA pass 3: neighbourhood blending of u_ldr with the weights (u_aa_b).
TEX(8) sampler2D u_ldr;
TEX(15) sampler2D u_aa_b;
VARY(0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 offset;
    SMAANeighborhoodBlendingVS(v_uv, offset);
    o_color = vec4(SMAANeighborhoodBlendingPS(v_uv, offset, u_ldr, u_aa_b).rgb, 1.0);
}
