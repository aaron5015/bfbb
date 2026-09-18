#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define WITH_D3D
#include "../rwbase.h"
#include "../rwplg.h"
#include "../rwerror.h"
#include "../rwrender.h"
#include "../rwengine.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "rwd3d.h"
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

// A buffer plus the system-memory copy callers lock against.
//
// D3D9's Lock hands out a pointer into the buffer and takes the writes back on
// Unlock, at any offset and for any part of it. D3D11 has nothing that does
// both: Map with WRITE_DISCARD throws away everything not rewritten, and
// UpdateSubresource takes a whole subresource. So the copy is what callers see
// and Unlock is what sends it, which also makes a partial lock work the way the
// pipelines expect.
struct D3d11Buffer
{
	ID3D11Buffer *buffer;
	uint8 *sys;
	uint32 length;
	// The extent of the outstanding lock, so unlock sends that and not the
	// whole buffer.
	uint32 lockOffset;
	uint32 lockSize;
	bool dynamic;
};

ID3D11Buffer*
bufferResource(void *buffer)
{
	return buffer ? ((D3d11Buffer*)buffer)->buffer : nil;
}

static void*
createBuffer(uint32 length, bool dynamic, UINT bindFlags)
{
	D3d11Buffer *b = rwNewT(D3d11Buffer, 1, MEMDUR_EVENT | ID_DRIVER);
	memset(b, 0, sizeof(*b));
	b->length = length;
	b->dynamic = dynamic;
	b->sys = rwNewT(uint8, length, MEMDUR_EVENT | ID_DRIVER);

	D3D11_BUFFER_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.ByteWidth = length;
	desc.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
	desc.BindFlags = bindFlags;
	desc.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;

	if(FAILED(d3d11device->CreateBuffer(&desc, nil, &b->buffer))){
		rwFree(b->sys);
		rwFree(b);
		return nil;
	}
	return b;
}

static void
destroyBuffer(void *buffer)
{
	D3d11Buffer *b = (D3d11Buffer*)buffer;
	if(b == nil)
		return;
	if(b->buffer)
		b->buffer->Release();
	rwFree(b->sys);
	rwFree(b);
}

static uint8*
lockBuffer(void *buffer, uint32 offset, uint32 size)
{
	D3d11Buffer *b = (D3d11Buffer*)buffer;
	if(size == 0)
		size = b->length - offset;
	b->lockOffset = offset;
	b->lockSize = size;
	return b->sys + offset;
}

static void
unlockBuffer(void *buffer)
{
	D3d11Buffer *b = (D3d11Buffer*)buffer;
	if(b == nil || b->buffer == nil || b->lockSize == 0)
		return;

	if(b->dynamic){
		// The whole buffer, because DISCARD is what makes a per-frame write
		// cheap and it leaves nothing of what was there before.
		D3D11_MAPPED_SUBRESOURCE map;
		if(SUCCEEDED(d3d11context->Map(b->buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))){
			memcpy(map.pData, b->sys, b->length);
			d3d11context->Unmap(b->buffer, 0);
		}
	}else{
		D3D11_BOX box;
		box.left = b->lockOffset;
		box.right = b->lockOffset + b->lockSize;
		box.top = 0;
		box.bottom = 1;
		box.front = 0;
		box.back = 1;
		d3d11context->UpdateSubresource(b->buffer, 0, &box, b->sys + b->lockOffset, 0, 0);
	}
	b->lockSize = 0;
}

void*
createVertexBuffer11(uint32 length, bool dynamic)
{
	void *b = createBuffer(length, dynamic, D3D11_BIND_VERTEX_BUFFER);
	if(b)
		d3d11Globals.numVertexBuffers++;
	return b;
}

void
destroyVertexBuffer11(void *buffer)
{
	if(buffer){
		forgetBuffer(buffer);
		destroyBuffer(buffer);
		d3d11Globals.numVertexBuffers--;
	}
}

void*
createIndexBuffer11(uint32 length, bool dynamic)
{
	void *b = createBuffer(length, dynamic, D3D11_BIND_INDEX_BUFFER);
	if(b)
		d3d11Globals.numIndexBuffers++;
	return b;
}

void
destroyIndexBuffer11(void *buffer)
{
	if(buffer){
		forgetBuffer(buffer);
		destroyBuffer(buffer);
		d3d11Globals.numIndexBuffers--;
	}
}

uint8 *lockBuffer11(void *buffer, uint32 offset, uint32 size) { return lockBuffer(buffer, offset, size); }
void unlockBuffer11(void *buffer) { unlockBuffer(buffer); }

#endif
}
}
}
