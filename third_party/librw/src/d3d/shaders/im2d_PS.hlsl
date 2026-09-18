#include "rwshader.h"

struct VS_out {
	float4 Position		: SV_POSITION;
	float3 TexCoord0	: TEXCOORD0;
	float4 Color		: COLOR0;
};

RW_TEXTURE(tex0, 0);

float4 fogColor : register(c0);

float4 main(VS_out input) : SV_Target
{
	float4 color = input.Color;
#ifdef TEX
	color *= RW_SAMPLE(tex0, input.TexCoord0.xy);
#endif
	RW_ALPHA_TEST(color.a);
	color.rgb = lerp(fogColor.rgb, color.rgb, input.TexCoord0.z);
	return color;
}
