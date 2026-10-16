// SMAA pass 2: blending weights from the edges (u_aa_a) and the area/search lookup textures.
TEX(14) sampler2D u_aa_a;
TEX(16) sampler2D u_area;
TEX(17) sampler2D u_search;
VARY(0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec2 pixcoord;
    vec4 offset[3];
    SMAABlendingWeightCalculationVS(v_uv, pixcoord, offset);
    o_color = SMAABlendingWeightCalculationPS(v_uv, pixcoord, offset, u_aa_a, u_area, u_search, vec4(0.0));
}
