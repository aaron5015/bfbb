#version 450

// A rounded box by its signed distance, in pixels: a circle when the corner
// radius reaches both half extents, and an outline when the stroke is set.

layout(location = 0) in vec2 vLocal;
layout(location = 1) in vec2 vHalf;
layout(location = 2) in vec2 vShape;
layout(location = 3) in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main()
{
	vec2 q = abs(vLocal) - (vHalf - vec2(vShape.x));
	float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - vShape.x;
	if(vShape.y > 0.0)
		d = abs(d + vShape.y * 0.5) - vShape.y * 0.5;
	float a = clamp(0.5 - d, 0.0, 1.0);
	outColor = vec4(vColor.rgb, vColor.a * a);
}
