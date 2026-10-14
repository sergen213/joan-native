#version 450
layout(push_constant) uniform Fill { vec4 color; } u_fill;  // linear premix, see shadow_fill_color()
layout(location = 0) out vec4 o_color;
void main() { o_color = u_fill.color; }
