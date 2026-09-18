#ifdef USE_UBOS
layout(std140) uniform State
{
	vec2 u_alphaRef;
	vec4  u_fogData;
	vec4  u_fogColor;
};
#else
uniform vec4 u_alphaRef;

uniform vec4  u_fogData;
uniform vec4  u_fogColor;
#endif

#define u_fogStart (u_fogData.x)
#define u_fogEnd (u_fogData.y)
#define u_fogRange (u_fogData.z)
#define u_fogDisable (u_fogData.w)

#ifndef GL2
out vec4 fragColor;
#endif

// The shadow map, and its knobs.
//
// u_shadowParams is (on, bias, strength, slope bias) and u_shadowParams2.x is
// one texel of the map in texture coordinates. The two bias terms and the texel
// are all handed over already converted into the map's own 0..1 depth units, so
// nothing here has to know how deep the light volume is. iShadowMap.cpp does
// that conversion, because it is the only thing that knows.
//
// tex2 and not a name of its own, because Shader::create binds tex0..tex3 to
// texture units 0..3 by name. Unit 0 is the material's texture and unit 1 is
// the environment map -- matfx_env.frag declares tex1 itself, so putting the
// shadow map there both redeclared the sampler and fought for the unit.
uniform sampler2D tex2;
uniform vec4 u_shadowParams;
uniform vec4 u_shadowParams2;
// Where the light travels, from it towards what it lights.
uniform vec4 u_shadowLightDir;

#define shadowEnabled (u_shadowParams.x)
#define shadowBias (u_shadowParams.y)
#define shadowStrength (u_shadowParams.z)
#define shadowSlopeBias (u_shadowParams.w)
#define shadowTexel (u_shadowParams2.x)

// Undo depth.frag's packing. The dot is the encode read backwards: each channel
// carries the fraction the ones before it could not.
float
UnpackDepth(vec4 c)
{
	return dot(c.rgb, vec3(1.0, 1.0/255.0, 1.0/65025.0));
}

// One comparison against one texel: 1.0 if the light reaches here.
float
ShadowTap(vec2 uv, float depth)
{
	return depth > UnpackDepth(texture(tex2, uv)) ? 0.0 : 1.0;
}

// 1.0 in light, shadowStrength in shadow, and the values between where the
// filter straddles an edge.
//
// Nine taps in a square, averaged. The map cannot be filtered by the hardware
// -- it holds depth packed across three bytes, and a linear filter would
// average the bytes of two unrelated depths into a number that is neither -- so
// the comparison happens first and the RESULTS are what get averaged. That is
// the whole of PCF, and it is why the taps are written out rather than done
// with a wider filter mode.
//
// The bounds test is not an optimisation. The map covers a slab of the world
// and CLAMP addressing means everything outside it samples the edge texel, so
// without this every surface beyond the volume takes whatever the border
// happens to hold -- the same trap xShadow.cpp's border comment describes, in
// the other direction.
float
ShadowLookup(vec4 shadowPos, float bias)
{
	if(shadowEnabled == 0.0)
		return 1.0;

	vec3 t = shadowPos.xyz*0.5 + 0.5;

	if(t.x < 0.0 || t.x > 1.0 || t.y < 0.0 || t.y > 1.0 || t.z > 1.0)
		return 1.0;

	float d = t.z - bias;
	float o = shadowTexel;

	float lit = ShadowTap(t.xy + vec2(-o, -o), d) +
	            ShadowTap(t.xy + vec2(0.0, -o), d) +
	            ShadowTap(t.xy + vec2( o, -o), d) +
	            ShadowTap(t.xy + vec2(-o, 0.0), d) +
	            ShadowTap(t.xy, d) +
	            ShadowTap(t.xy + vec2( o, 0.0), d) +
	            ShadowTap(t.xy + vec2(-o,  o), d) +
	            ShadowTap(t.xy + vec2(0.0,  o), d) +
	            ShadowTap(t.xy + vec2( o,  o), d);

	return mix(shadowStrength, 1.0, lit*(1.0/9.0));
}

// The test, given how squarely the surface faces the light. Two things come of
// that number, and both matter.
//
// A surface facing away from the light needs no map: it cannot see the light,
// and the lighting has already darkened it. Asking anyway is worse than
// pointless, because those are exactly the surfaces whose depth IS the map --
// the caster pass stores back faces -- so each one compares against its own
// record and breaks into stripes on the rounding.
//
// And a surface nearly edge-on to the light needs a larger bias than one facing
// it, because one texel of the map covers more depth the more the surface
// slopes away. tan of the angle to the light is exactly that ratio, which is
// the sqrt over ndl. Capped, or a surface at ninety degrees asks for infinity.
float
ShadowFactorV(vec4 shadowPos, float ndl)
{
	if(ndl <= 0.0)
		return 1.0;

	float slope = min(sqrt(max(1.0 - ndl*ndl, 0.0))/ndl, 8.0);

	return ShadowLookup(shadowPos, shadowBias + shadowSlopeBias*slope);
}

// The same, worked out here from a normal the fragment stage already has.
// Sharper than the interpolated number on a low-polygon model, which is where
// every character in this game sits.
//
// Takes the normal UNNORMALIZED, and that is the point. Geometry with no
// normals hands over a zero, normalize of which is a NaN, and a NaN reaching
// the comparisons below makes the result a driver's opinion rather than an
// answer -- header.vert's DoShadowNdl says more. A surface with no normal still
// needs its shadow, so it gets the test with the fixed slope allowance and no
// facing check.
float
ShadowFactorN(vec4 shadowPos, vec3 N)
{
	float len2 = dot(N, N);

	if(len2 < 1e-12)
		return ShadowLookup(shadowPos, shadowBias + shadowSlopeBias);

	return ShadowFactorV(shadowPos, dot(N, -u_shadowLightDir.xyz)*inversesqrt(len2));
}

// The outline's colour, with its thickness in world units in alpha.
uniform vec4 u_outlineColor;
uniform vec4 u_outlineColor2;
uniform vec4 u_outlineFlags;

// A light locked to the model rather than to the world, in xyz, with w saying
// whether to use it.
//
// **This is how the show lights a character and it is not how a renderer does.**
// An animator draws SpongeBob's front flat yellow and his side a solid darker
// green, and that stays true however he turns or wherever the sun is -- the
// shading describes the SHAPE, not the lighting. A world-space light cannot do
// that: turn the character and the dark side swings round with the room.
//
// So for a character the direction is taken from his own matrix instead, and
// travels straight back through him from the front. His face is then always in
// the top band and his sides always in the bottom one, whichever way he faces.
uniform vec4 u_toonLightDir;
// The colour of the room a character is standing in, with w saying whether to
// use it in place of his own lighting.
//
// A level lights its world and its objects with different rigs, and Rock Bottom
// is the case that shows why it matters: the room gets a blue kit summing to
// 0.83 1.52 1.77 and the characters get a grey one summing to 1.50 1.50 1.50.
// That is the game's own authoring and it is right for a renderer that draws
// characters as objects -- but a cartoon paints everything in a scene from the
// same palette, so a character standing in a blue room is drawn blue.
uniform vec4 u_toonRoomTint;

// The rest of the look, in two vectors because a uniform is four floats and
// there are seven things.
//
// **toonIsCharacter is the gate on everything that is not lighting.** The world
// and the characters take the same shading path, and most of what follows is
// wrong on a background: a painted backdrop does not want its colours rounded,
// does not want a rim, and has no baked occlusion worth reading. The scene sets
// it per draw, on the same call that hands over the room colour.
uniform vec4 u_toonExtra;
uniform vec4 u_toonExtra2;

#define toonColors (u_toonExtra.x)
#define toonRampRow (u_toonExtra.y)
#define toonIsCharacter (u_toonExtra.z)
#define toonWrap (u_toonExtra.w)

// The glint and the rim's blend, on a third vector: x how strong the glint is,
// y how far round the half vector it starts, z how the rim light is put on. See
// ToonGlossAmount, and setToonRimBlend in rwgl3.h.
uniform vec4 u_toonExtra3;

#define toonGloss (u_toonExtra3.x)
#define toonGlossEdge (u_toonExtra3.y)
#define toonRimBlend (u_toonExtra3.z)

#define toonRim (u_toonExtra2.x)
#define toonRimEdge (u_toonExtra2.y)
#define toonOcclusion (u_toonExtra2.z)
#define toonHardness (u_toonExtra2.w)


// The stylised look, off unless the application asks for it.
//
// x is on or off, y how many steps the light is cut into, z how far colour is
// pushed away from grey, w how strongly a surface facing away from the camera
// is lifted.
uniform vec4 u_toonParams;
// The stretch on a scaled ink. See outline.frag.
uniform vec4 u_outlineInk;

#define toonEnabled (u_toonParams.x)
// y was the band count and nothing has read it since the bands moved into the
// ramp texture. See the D3D9 twin in toonConstants.h.
#define toonModelShade (u_toonParams.y)
#define toonSaturation (u_toonParams.z)
#define toonStrength (u_toonParams.w)

// The colour ramp: what the light term looks up instead of being multiplied in
// directly.
//
// **This is the difference between a cel shade and a dimmer switch.** Banding
// the light arithmetically gives every step the same hue and only varies how
// much of it there is, so the shadow side of a character is the lit side turned
// down. A drawing does not do that -- its shadows shift towards blue while the
// lit side stays warm -- and no amount of arithmetic on a scalar can express
// that, because the colour has to come from somewhere.
//
// So the term indexes a strip of authored colour instead. Band count, band
// widths and band colours are all properties of the texture rather than of this
// shader, which is what lets a character be retuned without a rebuild.
//
// tex3, above the material texture, the environment map and the shadow map.
uniform sampler2D tex3;

// How many ramps are stacked in the strip. iToon.cpp builds them and the game
// picks a row per draw -- skin does not band like sheet metal.
#define TOON_RAMP_ROWS 4.0

vec3 ToonRampAt(float l)
{
	// Half a texel in on both axes, so the two ends of a row sample their own
	// colour rather than blending with the clamp, and a row samples itself
	// rather than the one above it.
	return texture(tex3, vec2(clamp(l, 0.02, 0.98),
	                          (toonRampRow + 0.5)/TOON_RAMP_ROWS)).rgb;
}

// The ramp, sampled across one pixel of the light term rather than at a point.
//
// **A band edge is a step function and a step function aliases.** The strip is
// point sampled on purpose -- filtering it would turn every band back into the
// gradient the bands exist to replace -- so the hard edge is real, and at one
// sample per pixel it crawls along a character as he walks. What is wanted is
// the edge kept hard in the SURFACE and resolved smoothly on the SCREEN, which
// is what a coverage estimate gives.
//
// fwidth is how much the light term changes between this pixel and the next, so
// four taps spread across it are four samples of what this pixel actually
// covers. Where the term is flat the taps land in the same band and nothing
// changes; where it crosses an edge they straddle it and the average is the
// fraction of the pixel on each side.
//
// Capped, because the generated world normals jump at seams and an uncapped
// spread there would smear a band across half a wall.
vec3 ToonRamp(float l)
{
	float w = min(fwidth(l), 0.08);

	return 0.25*(ToonRampAt(l - 0.375*w) + ToonRampAt(l - 0.125*w) +
	             ToonRampAt(l + 0.125*w) + ToonRampAt(l + 0.375*w));
}

// What the ramp is indexed by: the facing, wrapped, occluded and shadowed.
//
// **Everything that darkens a surface belongs on this side of the lookup.**
// Multiplying a shadow into the colour AFTER the ramp lays a smooth
// nine-tap gradient over a picture whose whole point is that it has none. Put
// in here instead, a cast shadow lands in the same flat band as the shading
// shadow and the two are indistinguishable -- which is what a drawing does with
// them.
// How much light the level's own models leave standing here. The D3D9 twin in
// toonConstants.h says why it scales the term before the ramp.
float ToonModelShade(vec3 prelit)
{
	if(toonModelShade <= 0.0)
		return 1.0;

	float v = max(prelit.r, max(prelit.g, prelit.b));

	return mix(1.0, v, toonModelShade);
}

float ToonLight(vec3 N, vec3 L, float occlusion, float shadow)
{
	float ndl = dot(N, -L);

	// Wrapped, if asked. max() collapses the whole far hemisphere to zero, so
	// the ramp has one value to say about all of it; the half-lambert remap
	// spreads that hemisphere over the bottom half of the strip instead and
	// gives it somewhere to put a second dark tone.
	float l = mix(max(0.0, ndl), 0.5 + 0.5*ndl, toonWrap);

	return clamp(l*occlusion*shadow, 0.0, 1.0);
}

// How much light the artists said reaches this vertex, as a scale on the term.
//
// The models carry a baked colour per vertex, and under a chin or inside a fold
// it is dark -- occlusion somebody drew, which no light rig recovers and which
// the toon path otherwise throws away whole. Reading it as a bias on the
// lookup is the same idea as the vertex-colour threshold offset every cel
// renderer of this kind has.
//
// Geometry with no baked colour reads as black, and black here would black the
// model out, so nothing is what an absent value means.
float ToonOcclusion(vec3 prelit)
{
	if(toonOcclusion <= 0.0 || toonIsCharacter == 0.0)
		return 1.0;

	float v = max(prelit.r, max(prelit.g, prelit.b));

	if(v < 1.0/255.0)
		return 1.0;

	return mix(1.0, v, toonOcclusion);
}

// How far round the silhouette this pixel is, as an amount to blend by.
//
// **Two things went wrong here and both are worth naming.**
//
// It took the HARDENED normal, which is worked out from screen derivatives and
// is therefore garbage on the one-pixel border where a 2x2 quad straddles two
// triangles. Every internal edge of the model got a wrong facing, fwidth of it
// came out huge, and the smoothstep below -- which is supposed to soften one
// pixel -- smeared the rim across whole patches of the face instead. A rim
// describes the SILHOUETTE, and a silhouette is a property of the real surface,
// so it takes the interpolated normal and always did want to.
//
// And it was ADDED to the lighting before the texture multiplied it, so a lit
// surface went past one, the texture scaled it further, and the result clipped.
// White blotches on SpongeBob, hardest where his texture was brightest. It is
// returned as an amount now and the caller blends the surface towards the
// colour of the room, which cannot leave the range however bright either is.
//
// Characters only. A rim on the world draws a bright line along every wall the
// camera happens to see edge-on.
// A panelled prop takes a third of it over a wide edge: the facing does not turn
// across a flat panel, so the smoothstep below would become a step and flip a
// whole face at once. See the D3D9 twin in toonConstants.h.
float ToonRimAmount(vec3 N, vec3 V)
{
	if(toonRim <= 0.0 || toonIsCharacter == 0.0)
		return 0.0;

	// Nothing on a face that points away from the eye: a model the game draws
	// two-sided shows plenty of them, and a rim describes the edge of a surface
	// the eye is looking at.
	float ndv = dot(N, normalize(V));

	if(ndv <= 0.0)
		return 0.0;

	float f = 1.0 - ndv;

	// Capped like ToonRamp's, and for the same reason: this is meant to soften
	// one pixel, and an uncapped spread turns it into a wash.
	float w = clamp(fwidth(f), 1.0/255.0, 0.05);

	return toonRim*smoothstep(toonRimEdge - w, toonRimEdge + w, f);
}

// The face's own normal, from how the surface moves across the triangle.
//
// **Welding took the hard edges out of the shading and this is what puts them
// back.** The hull needs one normal per position or the inflated copy cracks
// open at every corner, so iToon.cpp averages them -- and that same average is
// what lights the surface, which rounds off exactly the corners a drawing wants
// square. SpongeBob is a box; his corners should break, not blend.
//
// V is the vector to the eye, so its derivatives are the surface's negated, and
// the cross product of two negated vectors is unchanged. The sign is settled
// against the vertex normal below either way, because a derivative follows the
// winding and a normal does not.
//
// Not free of a trade, and worth stating: this hardens EVERY triangle boundary
// and not only the ones that were split, so a curved limb facets. The bands
// hide most of it. How far it goes is a setting because no shader can see which
// edges the artist meant.
vec3 ToonHardNormal(vec3 N, vec3 V)
{
	if(toonHardness <= 0.0 || toonIsCharacter == 0.0)
		return N;

	vec3 Ng = cross(dFdx(V), dFdy(V));
	float len2 = dot(Ng, Ng);

	if(len2 < 1e-20)
		return N;

	Ng *= inversesqrt(len2);

	if(dot(Ng, N) < 0.0)
		Ng = -Ng;

	return normalize(mix(N, Ng, toonHardness));
}


// Push colour away from grey.
//
// The show's palette is far more saturated than anything a light rig produces,
// and the levels were painted to match it before the bake flattened them out.
// Mixing AWAY from luminance -- a factor above one -- is the cheap way back:
// it leaves greys alone and pulls everything else outward.
//
// Applied after the texture, unlike the banding, because it is the artwork's
// colour that wants pushing and not the light's.
// How strongly light bounces off this pixel, as an amount to blend by.
//
// **A band and not a lobe.** A specular highlight is a gradient, and a gradient
// is the thing the cel look exists to remove -- so the half vector's facing is
// thresholded the way the ramp thresholds the light term, one step wide,
// antialiased across the pixel by its own derivatives. What that draws is a
// glint with an edge on it, which is how the show draws light on water.
//
// Not gated on toonIsCharacter: the surfaces that want this are not characters.
// The goo is the one that asks so far. See the D3D9 twin in toonConstants.h.
//
// L is where the light travels, so the direction towards it is its negative.
float ToonGlossAmount(vec3 N, vec3 V, vec3 L)
{
	if(toonGloss <= 0.0)
		return 0.0;

	vec3 H = normalize(normalize(V) - normalize(L));
	float s = dot(N, H);

	if(s <= 0.0)
		return 0.0;

	// Capped like ToonRamp's and ToonRimAmount's, and for the same reason.
	float w = clamp(fwidth(s), 1.0/255.0, 0.05);

	return toonGloss*smoothstep(toonGlossEdge - w, toonGlossEdge + w, s);
}

// Cut a colour down to a handful of shades, keeping the colour.
//
// **Rounding each channel on its own was the obvious way and it is wrong.** It
// moves the three channels by different amounts, so the HUE shifts as a colour
// lands: a face rounds towards pink in one band and towards orange in the
// next, which is banding that draws attention to itself rather than flatness
// that does not.
//
// Scaling instead. The brightest channel is rounded to a level and the whole
// colour is scaled by that ratio, so all three move together: hue and
// saturation come through untouched and only brightness is stepped. That is
// the axis the artwork actually varies along -- these textures were painted
// from a small palette and then shaded, and it is the shading that wants
// removing.
vec3 ToonQuantize(vec3 c)
{
	if(toonColors < 2.0)
		return c;

	float v = max(c.r, max(c.g, c.b));

	// Black has no hue to keep and no brightness to round.
	if(v < 1.0/255.0)
		return c;

	return c*(floor(v*toonColors + 0.5)/toonColors)/v;
}

// How far the colour can be pushed away from grey along one channel before
// that channel leaves 0..1.
//
// Solving l + g*d = 1 for a channel heading up and l + g*d = 0 for one heading
// down. A channel that is not moving constrains nothing.
float ToonGainLimit(float l, float d)
{
	if(d > 1.0/255.0)
		return (1.0 - l)/d;

	if(d < -1.0/255.0)
		return -l/d;

	return 1.0e6;
}

// Push colour away from grey WITHOUT moving how bright it is.
//
// **Three ways to do this and two of them cost you the picture.** Clipping each
// channel at one shifts the hue of exactly the colours the setting was turned
// up for -- a saturated yellow clips red and green together and drifts orange.
// Scaling the whole colour down to fit keeps the hue and takes the brightness
// with it, so the levels go dim precisely where they were most colourful, which
// is the opposite of what turning saturation up is for.
//
// What is actually wanted is the most chroma that still fits, at the brightness
// the surface already had. The luminance is held and the offset from it is
// grown until the first channel reaches an end of the range -- so a colour with
// room to move gets the full setting, and one that is already near the edge of
// what the display can show gets as much as it can take and stays as bright as
// it was.
vec3 ToonSaturate(vec3 c)
{
	if(toonEnabled == 0.0)
		return c;

	float l = dot(c, vec3(0.299, 0.587, 0.114));
	vec3 d = c - vec3(l);

	float g = min(toonSaturation,
	              min(ToonGainLimit(l, d.r),
	                  min(ToonGainLimit(l, d.g), ToonGainLimit(l, d.b))));

	// The dot of d with the same weights is zero, so adding a multiple of it
	// back cannot change the luminance. The clamp is against arithmetic, not
	// against the gain: that is already inside the range by construction.
	return clamp(vec3(l) + max(g, 0.0)*d, 0.0, 1.0);
}

void DoAlphaTest(float a)
{
#ifndef NO_ALPHATEST
	if(a < u_alphaRef.x || a >= u_alphaRef.y)
		discard;
#endif
}
