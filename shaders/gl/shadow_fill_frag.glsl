#version 330 core
uniform vec4 u_color;  // linear premix, see shadow_fill_color() in the renderer
layout(location = 0) out vec4 o_color;
// Remaster: the fill darkens the ambient MRT (SSAO subtracts its occluded share afterwards) and the auto exposure
// reference like the colour; the Original target has one colour buffer, so these outputs are dropped there.
layout(location = 1) out vec4 o_ambient;
layout(location = 3) out vec4 o_reference;
void main() { o_color = o_ambient = o_reference = u_color; }
