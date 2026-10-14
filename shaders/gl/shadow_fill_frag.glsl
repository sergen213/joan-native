#version 330 core
uniform vec4 u_color;  // linear premix, see shadow_fill_color() in the renderer
out vec4 o_color;
void main() { o_color = u_color; }
