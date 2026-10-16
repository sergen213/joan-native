// SMAA 1x configuration (third_party/smaa/SMAA.hlsl, MIT): included after remaster_common, before SMAA.hlsl.
// Runs at scene resolution on the sRGB-encoded LDR image; HIGH preset (threshold 0.1, 16 + 8 diagonal steps).
// The vertex-stage offset helpers run in the fragment stage (one shared full-screen vertex shader).
#define SMAA_GLSL_3
#define SMAA_RT_METRICS vec4(R.screen.zw, R.screen.xy)
#define SMAA_PRESET_HIGH
#define SMAA_INCLUDE_VS 1
#define SMAA_INCLUDE_PS 1
