#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stddef.h>

#define WITH_D3D
#include "../rwbase.h"
#include "../rwplg.h"
#include "../rwerror.h"
#include "../rwrender.h"
#include "../rwengine.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "rwd3d.h"
#include "rwd3d9.h"
#include "rwd3d11.h"
#include "rwd3dimpl.h"

#define PLUGIN_ID 0

namespace rw {
namespace d3d {

// The two device implementations live one namespace deeper than the interface
// they implement, so that a build can carry both. rw::d3d is the interface --
// the raster layer, the immediate mode and the pipelines are written against it
// and are built once -- and d3ddispatch.cpp forwards each of its entry points
// to whichever of these is running.
//
// A call in here that passes one of rw::d3d's enumerators has to name this
// namespace: the enumerator's own namespace is rw::d3d, so argument-dependent
// lookup adds the forwarder to the candidates beside the function meant, and
// the two have the same signature.
namespace impl11 {

#ifdef RW_D3D11

// Shader constants, kept as D3D9's constant register file.
//
// Every VSLOC_/PSLOC_ in this driver is a D3D9 c-register number and every
// shader is written against those numbers, so the register file is what the
// D3D11 backend keeps too: one array of float4 per stage, uploaded as a
// constant buffer when it has changed. The shaders declare their constants with
// packoffset at the same register, which is how HLSL spells a c-register inside
// a cbuffer.
// Up to c238: skin_matfx_env_VS.hlsl puts 64 bone matrices at c41 and the
// environment map constants after them. The pixel stage goes past ps_2_0's 32
// because the toon shaders are ps_3_0 on D3D9 and read c32; see toonConstants.h.
// The buffer has to be at least as large as the shader's $Globals, or the debug
// layer refuses the draw.
#define NUMVSCONST 256
#define NUMPSCONST 64
#define NUMVSINT 4

static float vsConstants[NUMVSCONST*4];
static float psConstants[NUMPSCONST*4];
static int32 vsIntConstants[NUMVSINT*4];

static bool32 vsConstantsDirty = 1;
static bool32 psConstantsDirty = 1;
static bool32 vsIntConstantsDirty = 1;

static ID3D11Buffer *vsConstantBuffer;
static ID3D11Buffer *psConstantBuffer;
static ID3D11Buffer *vsIntConstantBuffer;

// The pixel stage's alpha test, which D3D11 has no render state for. Kept at
// the top of the pixel register file, out of the way of the transient
// scratch registers c1..c3 that the port's own passes use.
#define PSLOC_alphaTest 7

static ID3D11Buffer*
createConstantBuffer(uint32 bytes)
{
	D3D11_BUFFER_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.ByteWidth = bytes;
	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	ID3D11Buffer *buf = nil;
	d3d11device->CreateBuffer(&desc, nil, &buf);
	return buf;
}

void
openShaderConstants(void)
{
	vsConstantBuffer = createConstantBuffer(sizeof(vsConstants));
	psConstantBuffer = createConstantBuffer(sizeof(psConstants));
	vsIntConstantBuffer = createConstantBuffer(sizeof(vsIntConstants));
	vsConstantsDirty = psConstantsDirty = vsIntConstantsDirty = 1;
}

void
closeShaderConstants(void)
{
	if(vsConstantBuffer){ vsConstantBuffer->Release(); vsConstantBuffer = nil; }
	if(psConstantBuffer){ psConstantBuffer->Release(); psConstantBuffer = nil; }
	if(vsIntConstantBuffer){ vsIntConstantBuffer->Release(); vsIntConstantBuffer = nil; }
}

static void
upload(ID3D11Buffer *buf, const void *data, uint32 bytes)
{
	if(buf == nil)
		return;
	D3D11_MAPPED_SUBRESOURCE map;
	if(SUCCEEDED(d3d11context->Map(buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))){
		memcpy(map.pData, data, bytes);
		d3d11context->Unmap(buf, 0);
	}
}

void
uploadShaderConstants(void)
{
	if(vsConstantsDirty){
		vsConstantsDirty = 0;
		upload(vsConstantBuffer, vsConstants, sizeof(vsConstants));
	}
	if(vsIntConstantsDirty){
		vsIntConstantsDirty = 0;
		upload(vsIntConstantBuffer, vsIntConstants, sizeof(vsIntConstants));
	}
	if(psConstantsDirty){
		psConstantsDirty = 0;
		upload(psConstantBuffer, psConstants, sizeof(psConstants));
	}
	ID3D11Buffer *vsbufs[2] = { vsConstantBuffer, vsIntConstantBuffer };
	d3d11context->VSSetConstantBuffers(0, 2, vsbufs);
	d3d11context->PSSetConstantBuffers(0, 1, &psConstantBuffer);
}

void
setVertexShaderConstantF(uint32 reg, const float32 *data, int32 numRegs)
{
	if(reg + numRegs > NUMVSCONST)
		return;
	memcpy(vsConstants + reg*4, data, numRegs*4*sizeof(float));
	vsConstantsDirty = 1;
}

void
setVertexShaderConstantI(uint32 reg, const int32 *data, int32 numRegs)
{
	if(reg + numRegs > NUMVSINT)
		return;
	memcpy(vsIntConstants + reg*4, data, numRegs*4*sizeof(int32));
	vsIntConstantsDirty = 1;
}

void
setPixelShaderConstantF(uint32 reg, const float32 *data, int32 numRegs)
{
	if(reg + numRegs > NUMPSCONST)
		return;
	memcpy(psConstants + reg*4, data, numRegs*4*sizeof(float));
	psConstantsDirty = 1;
}

// The alpha test, as the pixel shader sees it: the comparison as a number and
// the reference in 0..1. ALPHAALWAYS means no clip at all.
void
setAlphaTestConstants(uint32 func, uint32 ref)
{
	float c[4];
	c[0] = (float)func;
	c[1] = ref/255.0f;
	c[2] = 0.0f;
	c[3] = 0.0f;
	setPixelShaderConstantF(PSLOC_alphaTest, c, 1);
}

// --- shaders ----------------------------------------------------------------

// A vertex shader keeps its bytecode: D3D11 validates an input layout against
// the shader signature it will be drawn with, so the blob has to outlive
// creation.
struct VertexShader
{
	ID3D11VertexShader *shader;
	uint8 *code;
	uint32 codeSize;
};

// The compiled blobs the make_*.cmd scripts emit are DWORD arrays with no
// length beside them. A DXBC container carries its own total size at offset 24,
// which is where the length comes from.
static uint32
blobSize(const void *code)
{
	return ((const uint32*)code)[6];
}

void*
createVertexShader(void *csosrc)
{
	uint32 size = blobSize(csosrc);
	VertexShader *vs = rwNewT(VertexShader, 1, MEMDUR_EVENT | ID_DRIVER);
	memset(vs, 0, sizeof(*vs));
	if(FAILED(d3d11device->CreateVertexShader(csosrc, size, nil, &vs->shader))){
		rwFree(vs);
		return nil;
	}
	vs->code = rwNewT(uint8, size, MEMDUR_EVENT | ID_DRIVER);
	memcpy(vs->code, csosrc, size);
	vs->codeSize = size;
	d3d11Globals.numVertexShaders++;
	return vs;
}

void*
createPixelShader(void *csosrc)
{
	ID3D11PixelShader *ps = nil;
	if(FAILED(d3d11device->CreatePixelShader(csosrc, blobSize(csosrc), nil, &ps)))
		return nil;
	d3d11Globals.numPixelShaders++;
	return ps;
}

void
destroyVertexShader(void *shader)
{
	VertexShader *vs = (VertexShader*)shader;
	if(vs == nil)
		return;
	if(vs->shader)
		vs->shader->Release();
	rwFree(vs->code);
	rwFree(vs);
	d3d11Globals.numVertexShaders--;
}

void
destroyPixelShader(void *shader)
{
	if(shader){
		((ID3D11PixelShader*)shader)->Release();
		d3d11Globals.numPixelShaders--;
	}
}

// --- input layouts ----------------------------------------------------------

// A D3D9 vertex declaration is a description on its own; a D3D11 input layout
// is a description validated against one shader's input signature. So the
// declaration stays what the pipelines build and pass around -- d3d9.cpp's
// non-D3D9 arm already keeps it as a plain array -- and the layout is made when
// a declaration and a shader are first drawn together.
struct InputLayout
{
	void *declaration;
	void *vertexShader;
	ID3D11InputLayout *layout;
};

#define MAXLAYOUTS 128
static InputLayout layouts[MAXLAYOUTS];
static int32 numLayouts;
static int32 nextLayout;

static const char*
declUsageName(uint32 usage)
{
	switch(usage){
	case D3DDECLUSAGE_POSITION:	return "POSITION";
	case D3DDECLUSAGE_BLENDWEIGHT:	return "BLENDWEIGHT";
	case D3DDECLUSAGE_BLENDINDICES:	return "BLENDINDICES";
	case D3DDECLUSAGE_NORMAL:	return "NORMAL";
	case D3DDECLUSAGE_PSIZE:	return "PSIZE";
	case D3DDECLUSAGE_TEXCOORD:	return "TEXCOORD";
	case D3DDECLUSAGE_TANGENT:	return "TANGENT";
	case D3DDECLUSAGE_BINORMAL:	return "BINORMAL";
	case D3DDECLUSAGE_COLOR:	return "COLOR";
	}
	return "TEXCOORD";
}

static DXGI_FORMAT
declTypeFormat(uint32 type)
{
	switch(type){
	case D3DDECLTYPE_FLOAT1:	return DXGI_FORMAT_R32_FLOAT;
	case D3DDECLTYPE_FLOAT2:	return DXGI_FORMAT_R32G32_FLOAT;
	case D3DDECLTYPE_FLOAT3:	return DXGI_FORMAT_R32G32B32_FLOAT;
	case D3DDECLTYPE_FLOAT4:	return DXGI_FORMAT_R32G32B32A32_FLOAT;
	// D3DCOLOR is BGRA bytes; D3D11 has the swizzle in the format.
	case D3DDECLTYPE_D3DCOLOR:	return DXGI_FORMAT_B8G8R8A8_UNORM;
	case D3DDECLTYPE_UBYTE4:	return DXGI_FORMAT_R8G8B8A8_UINT;
	case D3DDECLTYPE_UBYTE4N:	return DXGI_FORMAT_R8G8B8A8_UNORM;
	case D3DDECLTYPE_SHORT2:	return DXGI_FORMAT_R16G16_SINT;
	case D3DDECLTYPE_SHORT4:	return DXGI_FORMAT_R16G16B16A16_SINT;
	case D3DDECLTYPE_SHORT2N:	return DXGI_FORMAT_R16G16_SNORM;
	case D3DDECLTYPE_SHORT4N:	return DXGI_FORMAT_R16G16B16A16_SNORM;
	case D3DDECLTYPE_USHORT2N:	return DXGI_FORMAT_R16G16_UNORM;
	case D3DDECLTYPE_USHORT4N:	return DXGI_FORMAT_R16G16B16A16_UNORM;
	case D3DDECLTYPE_FLOAT16_2:	return DXGI_FORMAT_R16G16_FLOAT;
	case D3DDECLTYPE_FLOAT16_4:	return DXGI_FORMAT_R16G16B16A16_FLOAT;
	}
	return DXGI_FORMAT_UNKNOWN;
}

// One semantic a vertex shader reads, from the input signature in its bytecode.
struct ShaderInput
{
	const char *name;
	uint32 index;
	uint32 componentType;	// D3D_REGISTER_COMPONENT_TYPE
	int32 numComponents;
};

static uint32
readU32(const uint8 *p)
{
	uint32 v;
	memcpy(&v, p, 4);
	return v;
}

// The ISGN chunk of a DXBC container: a count, the offset of the first entry,
// then 24 bytes an entry -- name offset, semantic index, system value, component
// type, register, mask. Names are offsets from the start of the chunk's data.
// System values are skipped: SV_VertexID is made by the runtime, not a layout.
static int32
readInputSignature(const uint8 *code, uint32 size, ShaderInput *out, int32 max)
{
	if(size < 32 || memcmp(code, "DXBC", 4) != 0)
		return 0;
	uint32 numChunks = readU32(code+28);
	for(uint32 c = 0; c < numChunks && 32 + (c+1)*4 <= size; c++){
		uint32 off = readU32(code + 32 + c*4);
		if(off + 16 > size || memcmp(code+off, "ISGN", 4) != 0)
			continue;
		const uint8 *data = code + off + 8;
		uint32 dataSize = readU32(code + off + 4);
		if(off + 8 + dataSize > size)
			return 0;
		uint32 count = readU32(data);
		uint32 first = readU32(data+4);
		int32 n = 0;
		for(uint32 i = 0; i < count && n < max; i++){
			const uint8 *e = data + first + i*24;
			if(first + (i+1)*24 > dataSize)
				break;
			if(readU32(e+8) != 0)
				continue;
			uint8 mask = e[20];
			out[n].name = (const char*)data + readU32(e);
			out[n].index = readU32(e+4);
			out[n].componentType = readU32(e+12);
			out[n].numComponents = (mask&1) + ((mask>>1)&1) + ((mask>>2)&1) + ((mask>>3)&1);
			n++;
		}
		return n;
	}
	return 0;
}

static DXGI_FORMAT
zeroInputFormat(const ShaderInput *in)
{
	static const DXGI_FORMAT floats[] = { DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT,
		DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT };
	static const DXGI_FORMAT uints[] = { DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32_UINT,
		DXGI_FORMAT_R32G32B32_UINT, DXGI_FORMAT_R32G32B32A32_UINT };
	static const DXGI_FORMAT sints[] = { DXGI_FORMAT_R32_SINT, DXGI_FORMAT_R32G32_SINT,
		DXGI_FORMAT_R32G32B32_SINT, DXGI_FORMAT_R32G32B32A32_SINT };
	int32 i = in->numComponents < 1 ? 0 : in->numComponents > 4 ? 3 : in->numComponents-1;
	switch(in->componentType){
	case D3D_REGISTER_COMPONENT_UINT32:	return uints[i];
	case D3D_REGISTER_COMPONENT_SINT32:	return sints[i];
	default:				return floats[i];
	}
}

ID3D11InputLayout*
inputLayoutFor(void *declaration, void *vertexShader)
{
	if(declaration == nil || vertexShader == nil)
		return nil;

	for(int32 i = 0; i < numLayouts; i++)
		if(layouts[i].declaration == declaration && layouts[i].vertexShader == vertexShader)
			return layouts[i].layout;

	d3d9::VertexElement *elements = (d3d9::VertexElement*)declaration;
	D3D11_INPUT_ELEMENT_DESC desc[16];
	int32 n = 0;
	for(; elements[n].stream != 0xFF && n < 16; n++){
		desc[n].SemanticName = declUsageName(elements[n].usage);
		desc[n].SemanticIndex = elements[n].usageIndex;
		desc[n].Format = declTypeFormat(elements[n].type);
		desc[n].InputSlot = elements[n].stream;
		desc[n].AlignedByteOffset = elements[n].offset;
		desc[n].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
		desc[n].InstanceDataStepRate = 0;
	}

	VertexShader *vs = (VertexShader*)vertexShader;

	// **What the shader reads and the geometry does not carry is fed zeroes.**
	// That is what D3D9 did with a missing element, and the shaders are written
	// against it: the hull reads its averaged normal from TEXCOORD1 and
	// TEXCOORD2 and falls back to the lighting normal where they read zero, and
	// only a model that has been through iToonHullNormals has either. D3D11
	// instead refuses to make the layout, and the draw is skipped. The zeroes
	// come from the constant vertex stream's texture coordinates, which are
	// zero and wide enough for any four-component input.
	ShaderInput inputs[16];
	int32 numInputs = readInputSignature(vs->code, vs->codeSize, inputs, 16);
	for(int32 j = 0; j < numInputs && n < 16; j++){
		bool32 present = 0;
		for(int32 k = 0; k < n && !present; k++)
			present = desc[k].SemanticIndex == inputs[j].index &&
			          _stricmp(desc[k].SemanticName, inputs[j].name) == 0;
		if(present)
			continue;
		desc[n].SemanticName = inputs[j].name;
		desc[n].SemanticIndex = inputs[j].index;
		desc[n].Format = zeroInputFormat(&inputs[j]);
		desc[n].InputSlot = 2;
		desc[n].AlignedByteOffset = offsetof(VertexConstantData, texCoors);
		desc[n].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
		desc[n].InstanceDataStepRate = 0;
		n++;
	}

	ID3D11InputLayout *layout = nil;
	if(FAILED(d3d11device->CreateInputLayout(desc, n, vs->code, vs->codeSize, &layout)))
		return nil;

	int32 i;
	if(numLayouts < MAXLAYOUTS)
		i = numLayouts++;
	else{
		// Round robin past the end, rather than leak the layout the caller is
		// about to bind. The context references what it binds, so an evicted
		// layout outlives the draw that is using it.
		i = nextLayout;
		nextLayout = (nextLayout+1) % MAXLAYOUTS;
		layouts[i].layout->Release();
	}
	layouts[i].declaration = declaration;
	layouts[i].vertexShader = vertexShader;
	layouts[i].layout = layout;
	return layout;
}

// A declaration is freed when the geometry that owns it is; anything made
// against it has to go with it.
void
forgetInputLayouts(void *declaration)
{
	for(int32 i = 0; i < numLayouts; ){
		if(layouts[i].declaration == declaration){
			layouts[i].layout->Release();
			layouts[i] = layouts[--numLayouts];
			nextLayout = 0;
		}else
			i++;
	}
}

void
releaseInputLayouts(void)
{
	for(int32 i = 0; i < numLayouts; i++)
		layouts[i].layout->Release();
	numLayouts = 0;
	nextLayout = 0;
}

ID3D11VertexShader*
vertexShaderResource(void *shader)
{
	return shader ? ((VertexShader*)shader)->shader : nil;
}

#endif
}
}
}
