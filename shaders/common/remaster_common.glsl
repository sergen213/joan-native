// Remaster (Enhanced/Ultra) shared declarations for the OpenGL and Vulkan backends.
// The including file first defines the API macros (shaders/gl/remaster_prelude.glsl, shaders/vk/remaster_vk.glsl):
//   UBO(b)  uniform-block qualifier, TEX(b) sampler qualifier, VK_BACKEND (1 = Vulkan image origin top-left).
// Every matrix here uses the OpenGL clip convention (z in [-1,1], y up); depth textures store (z+1)/2 in both APIs.
// Mirror: post_frame.hpp RemasterFrame (std140).

UBO(0) RemasterFrame {
    mat4 view_proj;        // jittered, GL convention (current frame)
    mat4 inv_view_proj;    // inverse of view_proj
    mat4 prev_view_proj;   // previous frame, unjittered
    mat4 view;             // world -> view
    mat4 shadow_matrix;    // world -> sun shadow clip
    vec4 sun_dir;          // xyz direction light travels; w shadow map on
    vec4 sun_color;        // rgb gamma (D3D); w exposure
    vec4 ambient;          // rgb gamma (D3D); w IBL on
    vec4 fog_color;        // rgb gamma (D3D); w volumetric density (0 = off)
    vec4 fog_range;        // x start, y end, z volumetric steps, w noise frame index (0 = static noise without TAA/RT)
    vec4 camera_pos;       // xyz; w SSAO on
    vec4 shadow_params;    // x 1/size, y PCF radius (texels), z distance, w normal offset (units)
    vec4 screen;           // xy scene size (px), zw 1/size
    vec4 jitter;           // xy jitter (NDC) this frame, z TAA history valid, w RCAS sharpness
    vec4 flags;            // x PBR, y normal maps, z particle lighting, w HDR tone mapping
    vec4 ssao;             // x radius, y intensity, z mesh texture LOD bias (-1 under TAA), w far
    vec4 env;              // x env mip count, y projection[1][1], z projection[0][0], w upscale ratio (output/scene)
} R;

const float kPi = 3.14159265;

vec3 to_linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }
vec3 to_srgb(vec3 c) {
    c = max(c, vec3(0.0));
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

// Texture/framebuffer uv <-> GL NDC xy. OpenGL images start at the bottom row, Vulkan at the top row.
vec2 uv_to_ndc(vec2 uv) {
#if VK_BACKEND
    return vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
#else
    return uv * 2.0 - 1.0;
#endif
}
vec2 ndc_to_uv(vec2 ndc) {
#if VK_BACKEND
    return vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
#else
    return ndc * 0.5 + 0.5;
#endif
}

vec3 world_from_depth(vec2 uv, float depth) {
    vec4 p = R.inv_view_proj * vec4(uv_to_ndc(uv), depth * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
}

// Interleaved gradient noise (Jimenez 2014), animated per frame for TAA.
float ign(vec2 pixel) {
    pixel += 5.588238 * mod(R.fog_range.w, 64.0);
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// Narkowicz ACES filmic fit.
vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
