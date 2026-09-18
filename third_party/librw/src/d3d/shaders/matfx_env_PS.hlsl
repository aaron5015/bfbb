#include "rwshader.h"

struct VS_out {
	float4 Position		: SV_POSITION;
	float3 TexCoord0	: TEXCOORD0;
	float2 TexCoord1	: TEXCOORD1;
	float4 Color		: COLOR0;
	float4 EnvColor		: COLOR1;
};

RW_TEXTURE(diffTex, 0);
RW_TEXTURE(envTex, 1);

float4 fogColor : register(c0);

float4 fxparams : register(c1);

#define shininess (fxparams.x)
#define disableFBA (fxparams.y)

float4 main(VS_out input) : SV_Target
{
	float4 pass1 = input.Color;
#ifdef TEX
	pass1 *= RW_SAMPLE(diffTex, input.TexCoord0.xy);
#endif
	RW_ALPHA_TEST(pass1.a);

	float4 pass2 = input.EnvColor*shininess*RW_SAMPLE(envTex, input.TexCoord1.xy);

	pass1.rgb = lerp(fogColor.rgb, pass1.rgb, input.TexCoord0.z);
	pass2.rgb = lerp(float3(0.0, 0.0, 0.0), pass2.rgb, input.TexCoord0.z);

	// We simulate drawing this in two passes.
	// First pass with standard blending, second with addition
	// We premultiply alpha so render state should be one.
	// For FB alpha rendering assume that diffuse alpha (pass1.a) was
	// written to framebuffer, so just multiply pass2 by it as well then.
	float fba = max(pass1.a, disableFBA);
	float4 color;
	color.rgb = pass1.rgb*pass1.a + pass2.rgb*fba;
	color.a = pass1.a;

	return color;
}
