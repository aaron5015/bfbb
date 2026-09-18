
//#define DIRECTIONALS
//#define POINTLIGHTS
//#define SPOTLIGHTS

#define ATTRIB_POS	0
#define ATTRIB_NORMAL	1
#define ATTRIB_COLOR	2
#define ATTRIB_WEIGHTS	3
#define ATTRIB_INDICES	4
#define ATTRIB_TEXCOORDS0	5
#define ATTRIB_TEXCOORDS1	6
#define ATTRIB_TEXCOORDS2	7


VSIN(ATTRIB_NORMAL)	vec3 in_normal;
VSIN(ATTRIB_COLOR)	vec4 in_color;
VSIN(ATTRIB_WEIGHTS)	vec4 in_weights;
VSIN(ATTRIB_INDICES)	vec4 in_indices;
VSIN(ATTRIB_TEXCOORDS0)	vec2 in_tex0;
// Sets one and two carry the hull's normal, (x,y) and (z,0), on a model that has
// been through iToonHullNormals. See the outline push in default.vert.
VSIN(ATTRIB_TEXCOORDS1)	vec2 in_tex1;
VSIN(ATTRIB_TEXCOORDS2)	vec2 in_tex2;


#ifdef USE_UBOS
layout(std140) uniform State
{
	vec2 u_alphaRef;
	vec4  u_fogData;
	vec4  u_fogColor;
};
#else
uniform vec4 u_alphaRef;
uniform vec4  u_fogData;
uniform vec4  u_fogColor;
#endif

#define u_fogStart (u_fogData.x)
#define u_fogEnd (u_fogData.y)
#define u_fogRange (u_fogData.z)
#define u_fogDisable (u_fogData.w)

#ifdef USE_UBOS
layout(std140) uniform Scene
{
	mat4 u_proj;
	mat4 u_view;
};
#else
uniform mat4 u_proj;
uniform mat4 u_view;
#endif

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

// The world matrix with its scale taken out -- see setWorldMatrix. Declared
// outside the Object block above so that the UBO and non-UBO builds share one
// declaration.
uniform mat4 u_normal;

uniform vec4 u_matColor;
uniform vec4 u_surfProps;	// amb, spec, diff, extra

// World position to shadow map, projection times view of the light camera.
// Outside the Object block on purpose: it changes once a frame rather than once
// an object, and the UBO and non-UBO builds then share one declaration.
uniform mat4 u_shadowMatrix;
// Where the light travels. Read here as well as in the fragment stage, so a
// build that lights per vertex can still hand the shadow test how squarely each
// surface faces the light -- without it, only the per-pixel build gets the
// facing check that keeps a caster from striping itself.
uniform vec4 u_shadowLightDir;

// How squarely a surface faces the light, for the shadow test.
//
// **Guarded against geometry that has no normals at all.** Two thirds of this
// game's levels ship a world without them -- it is prelit, so nothing ever
// needed one -- and where the attribute is missing it reads as zero. Normalize
// of a zero vector is a division by zero, and what comes out is a NaN.
//
// A NaN is not a small error here. Every comparison it reaches afterwards is
// undefined: the facing test, the slope, the depth compare. Which way each one
// falls is up to the driver, and the two answers are a whole level with no
// shadows or a whole level shadowed everywhere.
//
// The fallback is the cosine of forty-five degrees, which asks the test for
// exactly the fixed slope allowance it used before there was a normal here.
float DoShadowNdl(vec3 N)
{
	float len2 = dot(N, N);

	if(len2 < 1e-12)
		return 0.70710678;

	return dot(N, -u_shadowLightDir.xyz)*inversesqrt(len2);
}

// The outline's colour, with its thickness in world units in alpha.
uniform vec4 u_outlineColor;
// The second outline colour, and in alpha the object-space height below which
// it is used instead. SpongeBob's pants are drawn with a black line and the
// rest of him with a green one, which is how the show inks him.
uniform vec4 u_outlineColor2;
// Whether each ink is a colour in its own right (1) or a scale on the surface
// it surrounds (0). x is the upper region, y the lower.
uniform vec4 u_outlineFlags;
// Which way the hull inflates: x is +1 out of the surface, -1 into it, for a
// model wound inside out -- its normals point in, so pushing along them
// shrinks the copy instead of swelling it.
uniform vec4 u_outlineSign;

// A light locked to the model rather than to the world, in xyz, with w saying
// whether to use it.
//
// **This is how the show lights a character and it is not how a renderer does.**
// An animator draws SpongeBob's front flat yellow and his side a solid darker
// green, and that stays true however he turns or wherever the sun is -- the
// shading describes the SHAPE, not the lighting. A world-space light cannot do
// that: turn the character and the dark side swings round with the room.
//
// So for a character the direction is taken from his own matrix instead, and
// travels straight back through him from the front. His face is then always in
// the top band and his sides always in the bottom one, whichever way he faces.
uniform vec4 u_toonLightDir;
uniform vec4 u_toonRoomTint;
uniform vec4 u_toonExtra;

// The stylised look, off unless the application asks for it.
//
// x is on or off, y how many steps the light is cut into, z how far colour is
// pushed away from grey, w how strongly a surface facing away from the camera
// is lifted.
uniform vec4 u_toonParams;

#define toonEnabled (u_toonParams.x)
#define toonModelShade (u_toonParams.y)
#define toonSaturation (u_toonParams.z)
#define toonStrength (u_toonParams.w)


#define surfAmbient (u_surfProps.x)
#define surfSpecular (u_surfProps.y)
#define surfDiffuse (u_surfProps.z)

vec3 DoDynamicLight(vec3 V, vec3 N)
{
	vec3 color = vec3(0.0, 0.0, 0.0);
	for(int i = 0; i < MAX_LIGHTS; i++){
		if(u_lightParams[i].x == 0.0)
			break;
#ifdef DIRECTIONALS
		if(u_lightParams[i].x == 1.0){
			// direct. Plain: the stylised look is worked out per pixel, in
			// simple.frag, and does nothing at all with per_pixel_lighting off.
			float l = max(0.0, dot(N, -u_lightDirection[i].xyz));
			color += l*u_lightColor[i].rgb;
		}else
#endif
#ifdef POINTLIGHTS
		if(u_lightParams[i].x == 2.0){
			// point
			vec3 dir = V - u_lightPosition[i].xyz;
			float dist = length(dir);
			float atten = max(0.0, (1.0 - dist/u_lightParams[i].y));
			float l = max(0.0, dot(N, -normalize(dir)));
			color += l*u_lightColor[i].rgb*atten;
		}else
#endif
#ifdef SPOTLIGHTS
		if(u_lightParams[i].x == 3.0){
			// spot
			vec3 dir = V - u_lightPosition[i].xyz;
			float dist = length(dir);
			float atten = max(0.0, (1.0 - dist/u_lightParams[i].y));
			dir /= dist;
			float l = max(0.0, dot(N, -dir));
			float pcos = dot(dir, u_lightDirection[i].xyz);	// cos to point
			float ccos = -u_lightParams[i].z;
			float falloff = (pcos-ccos)/(1.0-ccos);
			if(falloff < 0.0)	// outside of cone
				l = 0.0;
			l *= max(falloff, u_lightParams[i].w);
			color += l*u_lightColor[i].rgb*atten;
		}else
#endif
			;
	}
	return color;
}

float DoFog(float w)
{
	return clamp((w - u_fogEnd)*u_fogRange, u_fogDisable, 1.0);
}
