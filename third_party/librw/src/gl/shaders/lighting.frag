// The lighting uniforms, for the fragment stage.
//
// header.vert declares these for the vertex stage and header.frag does not
// declare them at all, because until per-pixel lighting nothing in a fragment
// shader needed a light. This is prepended to the fragment shader of the
// per-pixel programs only; every other program is unchanged.
//
// Nothing has to be uploaded twice to make this work. A uniform of the same
// name declared in both stages is ONE uniform in the linked program, and
// gl3shader.cpp resolves every location by name against the whole program, so
// gl3device.cpp's existing setUniform calls reach here as they are.
//
// The Object block must match header.vert's member for member, including
// u_world and u_lightPosition, which nothing below reads. std140 lays a block
// out by declaration and not by use, so dropping the unused members would move
// the ones that are used.

#define MAX_LIGHTS 8

#ifdef USE_UBOS
layout(std140) uniform Object
{
	mat4  u_world;
	vec4  u_ambLight;
	vec4 u_lightParams[MAX_LIGHTS];	// type, radius, minusCosAngle, hardSpot
	vec4 u_lightPosition[MAX_LIGHTS];
	vec4 u_lightDirection[MAX_LIGHTS];
	vec4 u_lightColor[MAX_LIGHTS];
};
#else
uniform mat4 u_world;
uniform vec4 u_ambLight;
uniform vec4 u_lightParams[MAX_LIGHTS];	// type, radius, minusCosAngle, hardSpot
uniform vec4 u_lightPosition[MAX_LIGHTS];
uniform vec4 u_lightDirection[MAX_LIGHTS];
uniform vec4 u_lightColor[MAX_LIGHTS];
#endif

uniform vec4 u_matColor;
uniform vec4 u_surfProps;

#define surfAmbient (u_surfProps.x)
#define surfDiffuse (u_surfProps.z)

// Directional lights only, which is what the per-pixel path does on both
// backends. A light of any other type is a reason not to take this path at all
// -- gl3render.cpp decides that -- so the loop skips rather than handles them.
//
// The same equation as DoDynamicLight's directional arm in header.vert. The two
// have to stay identical or the setting changes more than where the maths
// happens.
// **The key direction and the room colour are not worked out here any more.**
//
// Both are per-draw quantities and neither depends on the normal, so looping
// over eight lights in every pixel to find them was eight comparisons per
// fragment for two numbers that do not change across the model. gl3device.cpp
// resolves them in setLights, where the lights are already in hand, and the
// shader reads u_toonLightDir and u_toonRoomTint. The D3D9 backend has always
// done it that way because ps_2_0 has no loops; this is GL3 catching up.

vec3 DoDynamicLightPP(vec3 N)
{
	vec3 color = vec3(0.0, 0.0, 0.0);
	for(int i = 0; i < MAX_LIGHTS; i++){
		if(u_lightParams[i].x == 0.0)
			break;
		if(u_lightParams[i].x == 1.0){
			// Plain, always. simple.frag builds the stylised shading on top
			// of what this returns, and a ramp applied here as well would be
			// a ramp applied twice.
			float l = max(0.0, dot(N, -u_lightDirection[i].xyz));
			color += l*u_lightColor[i].rgb;
		}
	}
	return color;
}
