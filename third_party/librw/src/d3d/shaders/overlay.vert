#version 450

// The present overlay: rounded boxes over the finished frame. Positions come
// in already in clip space; the rest passes through to overlay.frag.

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inLocal;
layout(location = 2) in vec2 inHalf;
layout(location = 3) in vec2 inShape;
layout(location = 4) in vec4 inColor;

layout(location = 0) out vec2 vLocal;
layout(location = 1) out vec2 vHalf;
layout(location = 2) out vec2 vShape;
layout(location = 3) out vec4 vColor;

void main()
{
	gl_Position = vec4(inPos, 0.0, 1.0);
	vLocal = inLocal;
	vHalf = inHalf;
	vShape = inShape;
	vColor = inColor;
}
