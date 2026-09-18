#include "rwshader.h"

// c0, as every pixel shader here has it.
float4 fogColor : register(c0);

#include "toonConstants.h"

// The ink, and how to read it: rgb is either a colour outright or a scale on
// the surface, and w says which. The vertex shader picks the region.
struct VS_out {
	float4 Position		: SV_POSITION;
	float3 TexCoord0	: TEXCOORD0;
	float4 Color		: COLOR0;
	float4 Outline		: TEXCOORD1;
};

// The stretch on a scaled ink: x how much colour it keeps, y a curve on its
// brightness. c2, which no other pixel shader in this set declares -- and it is
// uploaded immediately before the hull draws in any case.
float4 outlineInk : register(c2);

#define inkSaturation (outlineInk.x)
#define inkGamma (outlineInk.y)

// **A stretch, not a lift.** Multiplying the surface down gives an ink of the
// right hue and a washed-out one: a scale holds saturation where it was and
// takes brightness off everything, so a mid tone lands halfway to grey.
//
// Pushing the colour away from its own luminance puts the hue back, and a curve
// under one lifts the middle without moving either end. Both leave a genuine
// black alone -- there is no colour to push and no room to lift -- which is what
// keeps an ink that was black black.
float3 InkStretch(float3 c)
{
	float l = dot(c, float3(0.299, 0.587, 0.114));

	c = saturate(lerp(l.xxx, c, inkSaturation));

	return pow(c, inkGamma);
}

RW_TEXTURE(tex0, 0);

float4 main(VS_out input) : SV_Target
{
	// Two ways to read the ink, chosen per region.
	//
	// Scaled: a darkened copy of whatever the surface is painted, which gives
	// every character an ink on its own hue for nothing. Flat: a colour named
	// outright, for a region whose ink is not a darker version of itself --
	// SpongeBob's trousers are inked black whatever they are painted.
	// The stretch goes on the scaled variety only. A flat ink was named
	// outright, and a named colour is not something to second-guess.
	float4 tex = RW_SAMPLE(tex0, input.TexCoord0.xy);
	float3 ink = lerp(InkStretch(tex.rgb*input.Outline.rgb), input.Outline.rgb,
	                  input.Outline.a);

	// Lit like everything else. A line that stayed the same colour while the
	// surface it surrounds went blue reads as something laid over the scene
	// rather than part of it.
	// **The ink is as see-through as the surface it goes round.**
	//
	// Bubble Buddy is a bubble: a solid line round a transparent character
	// reads as a sticker on the glass. Taking the alpha from the texture and
	// the vertex costs nothing on an opaque model, where both are one.
	float4 color = float4(ink*toonRoom.rgb, tex.a*input.Color.a);

	RW_ALPHA_TEST(color.a);
	color.rgb = lerp(fogColor.rgb, color.rgb, input.TexCoord0.z);
	return color;
}
