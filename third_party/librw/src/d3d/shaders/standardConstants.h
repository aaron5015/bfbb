#define RW_VERTEX_STAGE
#include "rwshader.h"

float4x4	combinedMat	: register(c0);
float4x4	worldMat	: register(c4);
float3x3	normalMat	: register(c8);
float4		matCol		: register(c12);
float4		surfProps	: register(c13);
float4		fogData	: register(c14);
float4		ambientLight	: register(c15);

#define surfAmbient (surfProps.x)
#define surfSpecular (surfProps.y)
#define surfDiffuse (surfProps.z)

#define fogStart (fogData.x)
#define fogEnd (fogData.y)
#define fogRange (fogData.z)
#define fogDisable (fogData.w)

#include "lighting.h"

#ifdef SM4
// The counts, one to a register because that is how setNumLights uploads them.
cbuffer VSIntConstants : register(b1)
{
	int4 dirLightCount	: packoffset(c0);
	int4 pointLightCount	: packoffset(c1);
	int4 spotLightCount	: packoffset(c2);
};

#define numDirLights (dirLightCount.x)
#define numPointLights (pointLightCount.x)
#define numSpotLights (spotLightCount.x)

// A float register at SM4 holds float bits, and an int4 declared over it would
// read them as integers.
float4 firstLight : register(c16);

#define firstDirLight ((int)firstLight.x)
#define firstPointLight ((int)firstLight.y)
#define firstSpotLight ((int)firstLight.z)
#else
int numDirLights : register(i0);
int numPointLights : register(i1);
int numSpotLights : register(i2);
int4 firstLight : register(c16);

#define firstDirLight (firstLight.x)
#define firstPointLight (firstLight.y)
#define firstSpotLight (firstLight.z)
#endif

Light lights[8] : register(c17);
