// The inverted hull: flat colour, nothing else.
//
// This is the second half of the oldest trick for a cartoon outline. The model
// is drawn twice: once inflated along its normals with the FRONT faces culled,
// which leaves only the parts of the swollen copy that stick out past the real
// one -- a band around the silhouette -- and once normally on top. There is no
// edge detection anywhere; the outline is a shape, not a filter, which is why
// it survives at any resolution and costs one extra draw.
//
// Note for anyone editing this file: it becomes a C string literal one line at
// a time, through a sed recipe that does not escape anything. A double quote
// character here is a compile error in the generated .inc.

uniform sampler2D tex0;

FSIN vec4 v_color;
FSIN vec2 v_tex0;
FSIN float v_fog;
FSIN vec4 v_shadowPos;
FSIN vec4 v_outline;
// How squarely the hull faces the light, for the shadow test. Worked out in the
// vertex shader from the same normal the inflation used.
FSIN float v_shadowNdl;

// A stretch on a scaled ink: x how much colour it keeps, y a curve on its
// brightness. The D3D9 twin in outline_PS.hlsl says why a plain multiply washes
// an ink out and why this leaves a black one alone.
vec3 InkStretch(vec3 c)
{
	float l = dot(c, vec3(0.299, 0.587, 0.114));

	c = clamp(mix(vec3(l), c, u_outlineInk.x), 0.0, 1.0);

	return pow(c, vec3(u_outlineInk.y));
}

void
main(void)
{
	// **The ink is the surface's own colour, darkened.** A flat black line
	// round everything reads as a diagram; the show inks each part of a
	// character in a darker version of what that part is painted, which is why
	// SpongeBob's edge is olive against his yellow and not black.
	//
	// So the hull samples the very texture the model is about to be drawn with
	// and multiplies it down. v_outline carries how far down, per region, which
	// is what lets his trousers still come out black -- a scale of zero is
	// black whatever the texture underneath says.
	// Two ways to read the ink, chosen per region.
	//
	// Scaled: a darkened copy of whatever the surface is painted, which is what
	// gives every character an ink on its own hue for nothing.
	//
	// Flat: a colour named outright, for where a character has an ink that is
	// not simply a darker version of himself. SpongeBob is the case -- the show
	// draws him with the same green his holes are, which is nowhere near a
	// darkened yellow.
	// The stretch goes on the scaled variety only. A flat ink was named
	// outright, and a named colour is not something to second-guess.
	vec4 tex = texture(tex0, vec2(v_tex0.x, 1.0-v_tex0.y));
	vec3 ink = mix(InkStretch(tex.rgb*v_outline.rgb), v_outline.rgb, v_outline.a);

	// **The ink is lit like everything else.** An outline is drawn in ink that
	// belongs to the picture, not stamped on top of it, so a line that stayed
	// the same colour while the surface it surrounds went blue would read as
	// something laid over the scene rather than part of it. Flat across the
	// model, like the shading it borders.
	//
	// Resolved by setLights before the draw, as the shading's is.
	vec3 room = u_toonRoomTint.rgb;

	// And shadowed like everything else, for the same reason: a line that
	// stayed lit while the character it surrounds walked into shade would
	// separate from him.
	//
	// The facing test inside ShadowFactorV is doing real work here. The hull is
	// drawn with its front faces culled, so what is visible is its far side and
	// the far side is what the caster pass recorded -- every fragment facing
	// away from the light would compare against its own record and break into
	// stripes. Those fragments are returned lit instead.
	// **The ink is as see-through as the surface it goes round.**
	//
	// Bubble Buddy is a bubble: a solid line round a transparent character
	// reads as a sticker on the glass. Taking the alpha from the texture and
	// the vertex costs nothing on an opaque model, where both are one.
	vec4 color = vec4(ink*room*ShadowFactorV(v_shadowPos, v_shadowNdl), tex.a*v_color.a);

	// Into the fog like everything else. An outline that stayed black as the
	// model behind it faded would draw a hard shape around a ghost.
	color.rgb = mix(u_fogColor.rgb, color.rgb, v_fog);

	FRAGCOLOR(color);
}
