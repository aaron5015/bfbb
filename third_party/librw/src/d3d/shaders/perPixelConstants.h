// Pixel shader constants for the per-pixel lighting path.
//
// These mirror what standardConstants.h puts in vertex shader registers. They
// are a second copy rather than a move because the per-vertex path is still
// there and still the default: both shaders are compiled, and which one runs is
// a setting.
//
// The register numbers start at 8 and not at 1 for a reason worth keeping. The
// pixel shader constants are device state, shared by every pixel shader the
// frame runs, and c1 upwards belongs to whoever is drawing at the time -- the
// matfx shader's shininess is c1, the port's glow pass takes c1 to c3 and its
// distortion pass takes c1. matCol and surfProps below are uploaded only when
// the material changes, so a pass that wrote over them would not be corrected
// until the material next changed, which could be a whole level later. Leaving
// c1 to c7 alone is what stops that.
//
// ps_2_0 has 32 float constants and this ends at c26. Adding a ninth light does
// not fit; see MAX_LIGHTS in d3drender.cpp.

float4 fogColor : register(c0);

float4 ppMatCol : register(c8);
float4 ppSurfProps : register(c9);
float4 ppAmbient : register(c10);

// Colour and direction split into two arrays instead of one array of the Light
// struct in lighting.h: the per-pixel path only ever does directional lights,
// so a light's position register would be uploaded and never read.
float4 ppLightColor[8] : register(c11);
float4 ppLightDir[8] : register(c19);

#define ppSurfAmbient (ppSurfProps.x)
#define ppSurfDiffuse (ppSurfProps.z)
