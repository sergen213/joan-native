#version 460
// Remaster mesh fragment shader with the ray-traced effects (docs/RAYTRACING.md): G-buffer prepass (pass 2),
// RT sun visibility / reflections from the screen-space RT targets, DDGI ambient, inline water reflections.
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#define RAY_TRACED 1
#include "mesh_pbr.glsl"
