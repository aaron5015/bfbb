struct Light
{
	float4 color;	// and radius
	float4 position;	// and -cos(angle)
	float4 direction;	// and falloff clamp
};

float3 DoDirLight(Light L, float3 N)
{
	float l = max(0.0, dot(N, -L.direction.xyz));
	return l*L.color.xyz;
}

// The same light, for the pixel shader, which is handed colour and direction
// loose rather than as a Light -- see perPixelConstants.h. Kept next to the one
// above so the two cannot drift apart unnoticed: they must stay the same
// equation or the per-pixel setting changes more than where the maths happens.
float3 DoDirLightPP(float3 color, float3 direction, float3 N)
{
	float l = max(0.0, dot(N, -direction));
	return l*color;
}

float3 DoDirLightSpec(Light L, float3 N, float3 V, float power)
{
	return pow(saturate(dot(N, normalize(V + -L.direction.xyz))), power)*L.color.xyz;
}

float3 DoPointLight(Light L, float3 V, float3 N)
{
	// As on PS2
	float3 dir = V - L.position.xyz;
	float dist = length(dir);
	float atten = max(0.0, (1.0 - dist/L.color.w));
	float l = max(0.0, dot(N, -normalize(dir)));
	return l*L.color.xyz*atten;
}

float3 DoSpotLight(Light L, float3 V, float3 N)
{
	// As on PS2
	float3 dir = V - L.position.xyz;
	float dist = length(dir);
	float atten = max(0.0, (1.0 - dist/L.color.w));
	dir /= dist;
	float l = max(0.0, dot(N, -dir));
	float pcos = dot(dir, L.direction.xyz);	// cos to point
	float ccos = -L.position.w;	// cos of cone
	float falloff = (pcos-ccos)/(1.0-ccos);
	if(falloff < 0)	// outside of cone
		l = 0;
	l *= max(falloff, L.direction.w);	// falloff clamp
	return l*L.color.xyz*atten;
}
