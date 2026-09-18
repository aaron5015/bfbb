#include "rwshader.h"

// TOON takes perPixelConstants too: it wants the material colour and the fog
// colour, and it needs the same normal the per-pixel path does.
#if defined(PERPIXEL) || defined(TOON)
#include "perPixelConstants.h"
#include "lighting.h"
#else
float4 fogColor : register(c0);
#endif

#ifdef TOON
#include "toonConstants.h"
#endif

struct VS_out {
	float4 Position		: SV_POSITION;
	float3 TexCoord0	: TEXCOORD0;
	float4 Color		: COLOR0;
#if defined(PERPIXEL) || defined(TOON)
	float3 Normal		: TEXCOORD1;
#endif
#ifdef TOON
	// From the surface towards the eye, world space. The rim light needs it,
	// and ToonHardNormal takes its derivatives as a stand-in for the surface's
	// own. Written by the per-pixel vertex shader, which is the one the toon
	// path runs with.
	float3 ViewDir		: TEXCOORD2;
#endif
};

RW_TEXTURE(tex0, 0);

float4 main(VS_out input) : SV_Target
{
	float4 color = input.Color;

	// Held rather than applied where they are found: both are wanted after the
	// texture. See simple.frag on the GL3 side.
	float3 toonRoomC = float3(1.0, 1.0, 1.0);
	float toonRimAmt = 0.0;
	float toonGlossAmt = 0.0;

#ifdef TOON
	// **The lighting is replaced, not shaded on top of.**
	//
	// The room contributes its colour and nothing else, flat across the model,
	// so a cave darkens the shadow band and Rock Bottom turns it blue without
	// putting back the smooth falloff the bands exist to remove. See
	// simple.frag on the GL3 side; this is the same arithmetic.
	float3 N = normalize(input.Normal);

	// The shading normal, hardened back towards the face's own where the
	// setting asks -- welding softened corners that should break.
	float3 Ns = ToonHardNormal(N, input.ViewDir);

	float3 cel = ToonRamp(ToonLight(Ns, toonLightDir.xyz,
	                                ToonOcclusion(input.Color.rgb) *
	                                ToonModelShade(input.Color.rgb)));

	color.rgb = toonRoom.rgb * lerp(float3(1.0, 1.0, 1.0), cel, toonStrength);

	// **N and not Ns.** The rim wants the real surface; the hardened normal is
	// for the bands. ToonRimAmount says what that cost.
	toonRoomC = toonRoom.rgb;
	toonRimAmt = ToonRimAmount(N, input.ViewDir);
	toonGlossAmt = ToonGlossAmount(N, input.ViewDir, toonLightDir.xyz);
	color.a *= ppMatCol.a;
#elif defined(PERPIXEL)
	// The vertex shader handed over the prelight and a normal and did nothing
	// else. What follows is default_VS.hlsl's lighting, in the same order and
	// with the same clamp, evaluated here instead.
	float3 N = normalize(input.Normal);

	color.rgb += ppAmbient.rgb * ppSurfAmbient;

	// Eight lights, always. ps_2_0 has no loops and no branches, so the count
	// cannot be a constant the shader reads -- every slot is evaluated and a
	// slot that holds no light is one whose colour d3drender.cpp uploaded as
	// zero. That is about twenty-four instructions of the sixty-four available.
	float3 lit = float3(0.0, 0.0, 0.0);
	for(int i = 0; i < 8; i++)
		lit += DoDirLightPP(ppLightColor[i].rgb, ppLightDir[i].xyz, N);

	// One multiply by the diffuse coefficient rather than one per light. The
	// vertex path scales each light as it sums them; the coefficient is scalar,
	// so the two are the same sum.
	color.rgb += lit * ppSurfDiffuse;

	// PS2 clamps before material color
	color = clamp(color, 0.0, 1.0);
	color *= ppMatCol;
#endif

#ifdef TEX
	color *= RW_SAMPLE(tex0, input.TexCoord0.xy);
#endif
#ifdef TOON
	// The silhouette light, after the texture.
	//
	// **Three ways to put it on, because a rim is light and not paint.** Towards
	// the room's colour replaces what is there: at full amount the band IS the
	// room, so whatever the surface was doing stops at its edge. Screening keeps
	// it -- brightening by what is left of the range rather than by a fixed
	// amount, so the texture still reads through the band -- and cannot leave
	// the range however bright either side already is. Adding is the brightest
	// of the three and the only one that clips, which is what it looked like
	// before any of this was a blend at all.
	float3 rimLight = toonRimAmt*toonRoomC;

	if(toonRimBlend >= 1.5)
		color.rgb = saturate(color.rgb + rimLight);
	else if(toonRimBlend >= 0.5)
		color.rgb = 1.0 - (1.0 - color.rgb)*(1.0 - rimLight);
	else
		color.rgb = lerp(color.rgb, toonRoomC, toonRimAmt);

	// The glint, over everything the surface is. Towards white and not towards
	// the room, unlike the rim: a highlight is the light itself, where a rim is
	// the room seen along an edge.
	color.rgb = lerp(color.rgb, float3(1.0, 1.0, 1.0), toonGlossAmt);

	color.rgb = ToonSaturate(color.rgb);

	// **Flattening is the last thing that happens to the colour.**
	//
	// It used to run straight after the texture, which left the saturation to
	// scale it off the levels it had just been rounded to: the setting asked
	// for twelve shades and the frame buffer got however many came out the
	// other end. Fog is the only thing allowed after, because fog is the air
	// and not the surface.
	//
	// A character and not the world -- a painted background does not want its
	// colours rounded.
	if(toonIsCharacter != 0.0)
		color.rgb = ToonQuantize(color.rgb);
#endif

	RW_ALPHA_TEST(color.a);
	color.rgb = lerp(fogColor.rgb, color.rgb, input.TexCoord0.z);
	return color;
}
