#version 450
// Remaster shadow fill: darkens the HDR colour, the ambient MRT (so SSAO, which subtracts the occluded ambient
// afterwards, does not remove more than the shadowed pixel still holds) and the auto exposure reference alike.
layout(push_constant) uniform Fill { vec4 color; } u_fill;  // linear premix, see shadow_fill_color()
layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_ambient;
layout(location = 3) out vec4 o_reference;
void main() { o_color = o_ambient = o_reference = u_fill.color; }
