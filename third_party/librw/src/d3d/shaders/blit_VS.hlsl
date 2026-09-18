// The virtual screen, on its way to the back buffer.
//
// One triangle big enough to cover the target, generated from the vertex index:
// no vertex buffer, no input layout, nothing to bind but the shaders and the
// texture. Where the picture lands is the viewport's business, which is what
// keeps the aspect ratio.

struct VS_out {
	float4 Position	: SV_POSITION;
	float2 TexCoord	: TEXCOORD0;
};

VS_out main(uint id : SV_VertexID)
{
	VS_out output;
	output.TexCoord = float2((id << 1) & 2, id & 2);
	output.Position = float4(output.TexCoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	return output;
}
