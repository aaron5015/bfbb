// What differs between the two shader models these sources compile to.
//
// Every .hlsl in this directory is compiled twice by make_shaders.cmd: once at
// shader model 2 or 3 into shaders/ for the D3D9 backend, and once at shader
// model 4 into shaders11/ with SM4 defined for the D3D11 backend. One source is
// what keeps the two backends drawing the same picture; this header is the whole
// of what is allowed to differ.
//
// **The constant register file is the same in both.** At shader model 4, fxc
// takes register(cN) on a global as its offset in the $Globals constant buffer,
// which is register b0 -- so a constant declared at c27 here is at c27 in the
// D3D9 register file and at byte 27*16 of the buffer the D3D11 backend uploads.
// Nothing about constants needs a macro.
//
// What does:
//
// * A texture. SM4 splits a sampler into a texture and a sampler state. fxc's
//   /Gec would accept sampler2D and tex2D at SM4, but it binds every such
//   texture to t0 whatever register the sampler names.
// * The alpha test. D3D11 has no render state for it, so every SM4 pixel shader
//   clips against c7, which the D3D11 backend uploads.
// * Integer constants. D3D9 keeps them in a register file of their own; the
//   D3D11 backend uploads them as a second constant buffer at b1.
// * Blend indices, which SM4 reads as unsigned.

#ifdef SM4

#define RW_TEXTURE(name, n) \
	Texture2D name : register(t##n); \
	SamplerState name##Sampler : register(s##n)
#define RW_SAMPLE(name, uv) name.Sample(name##Sampler, uv)

#define RW_BLENDINDICES uint4

// rw::AlphaTestFunc. D3D11's setAlphaTestConstants uploads the function and the
// reference; ALPHAALWAYS means no clip at all.
//
// Pixel shaders only. c7 is inside worldMat in the vertex register file, and
// fxc refuses two globals at one register whether or not either is read, so
// standardConstants.h says which stage it is.
#ifndef RW_VERTEX_STAGE
float4 rwAlphaTest : register(c7);

void RW_ALPHA_TEST(float alpha)
{
	int func = (int)rwAlphaTest.x;
	if(func == 1)		// ALPHAGREATEREQUAL
		clip(alpha - rwAlphaTest.y);
	else if(func == 2)	// ALPHALESS
		clip(rwAlphaTest.y - alpha - 1.0/512.0);
}
#endif

#else

#define RW_TEXTURE(name, n) sampler2D name : register(s##n)
#define RW_SAMPLE(name, uv) tex2D(name, uv)

#define RW_BLENDINDICES int4

// The output merger does it, from D3DRS_ALPHAFUNC.
#define RW_ALPHA_TEST(alpha)

#endif
