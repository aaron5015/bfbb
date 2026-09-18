#include "standardConstants.h"

// skin_VS.hlsl and matfx_env_VS.hlsl both want the registers right after the
// lights, and there is only one of each: the bone matrices are 192 of them, so
// they stay where the plain skinning shader has them and the environment map
// constants move to the far end. That way uploadSkinMatrices is the same call
// for both skinning shaders, and only the env constants need a base of their
// own -- which is what d3d9skinmatfx.cpp passes to uploadEnvMapState.
float4x3	boneMatrices[64] : register(c41);
float4x4	texMat	: register(c233);
float4		colorClamp : register(c237);
float4		envColor : register(c238);

struct VS_in
{
	float4 Position		: POSITION;
	float3 Normal		: NORMAL;
	float2 TexCoord		: TEXCOORD0;
	float4 Prelight		: COLOR0;
	float4 Weights		: BLENDWEIGHT;
	RW_BLENDINDICES Indices	: BLENDINDICES;
};

struct VS_out {
	float4 Position		: SV_POSITION;
	float3 TexCoord0	: TEXCOORD0;	// also fog
	float2 TexCoord1	: TEXCOORD1;
	float4 Color		: COLOR0;
	float4 EnvColor		: COLOR1;
};


VS_out main(in VS_in input)
{
	VS_out output;

	int j;
	float3 SkinVertex = float3(0.0, 0.0, 0.0);
	float3 SkinNormal = float3(0.0, 0.0, 0.0);
	for(j = 0; j < 4; j++){
		SkinVertex += mul(input.Position, boneMatrices[input.Indices[j]]).xyz * input.Weights[j];
		SkinNormal += mul(input.Normal, (float3x3)boneMatrices[input.Indices[j]]).xyz * input.Weights[j];
	}

	output.Position = mul(combinedMat, float4(SkinVertex, 1.0));
	float3 V = mul(worldMat, float4(SkinVertex, 1.0)).xyz;
	float3 N = mul(normalMat, SkinNormal);

	output.TexCoord0.xy = input.TexCoord;
	// The env map coordinate comes off the SKINNED normal, which is the whole
	// point of this shader: a bone-deformed model has to reflect what its
	// deformed surface faces, not what its bind pose did.
	output.TexCoord1 = mul(texMat, float4(N, 1.0)).xy;

	output.Color = input.Prelight;
	output.Color.rgb += ambientLight.rgb * surfAmbient;

	int i;
#ifdef DIRECTIONALS
	for(i = 0; i < numDirLights; i++)
		output.Color.xyz += DoDirLight(lights[i+firstDirLight], N)*surfDiffuse;
#endif
#ifdef POINTLIGHTS
	for(i = 0; i < numPointLights; i++)
		output.Color.xyz += DoPointLight(lights[i+firstPointLight], V, N)*surfDiffuse;
#endif
#ifdef SPOTLIGHTS
	for(i = 0; i < numSpotLights; i++)
		output.Color.xyz += DoSpotLight(lights[i+firstSpotLight], V, N)*surfDiffuse;
#endif
	// PS2 clamps before material color
	output.Color = clamp(output.Color, 0.0, 1.0);
	output.EnvColor = max(output.Color, colorClamp) * envColor;
	output.Color *= matCol;

	output.TexCoord0.z = clamp((output.Position.w - fogEnd)*fogRange, fogDisable, 1.0);

	return output;
}
