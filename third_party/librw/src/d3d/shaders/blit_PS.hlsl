struct VS_out {
	float4 Position	: SV_POSITION;
	float2 TexCoord	: TEXCOORD0;
};

Texture2D screen : register(t0);
SamplerState screenSampler : register(s0);

float4 main(VS_out input) : SV_TARGET
{
	return float4(screen.Sample(screenSampler, input.TexCoord).rgb, 1.0);
}
