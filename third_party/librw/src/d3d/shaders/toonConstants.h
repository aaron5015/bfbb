// The cel look, in pixel shader constants.
//
// c27 to c29, above perPixelConstants.h's c8..c26 and well above the c1..c7
// that other passes scribble on. ps_2_0 allows 32.
//
// **Both the key direction and the room colour arrive already worked out.**
// The GL3 version finds them by looping over the light array in the shader,
// which ps_2_0 cannot do -- it has neither loops nor branches, so eight lights
// means eight unrolled comparisons in every pixel. They are per-draw
// quantities and not per-pixel ones, so d3drender.cpp computes them once on
// the way to the uniform and what is left here is a dot, a lookup and two
// multiplies. That is why the toon path is CHEAPER than the lighting it
// replaces rather than an addition to it.

float4 toonParams : register(c27);
// Where the light travels, already resolved: the character's own front, the
// camera, or the room's brightest light, whichever the setting asked for.
float4 toonLightDir : register(c28);
// What colour it is in here, with no direction in it.
float4 toonRoom : register(c29);
// The rest of the look, in two registers because there are seven things and a
// register is four floats.
//
// **toonIsCharacter gates everything that is not lighting.** The world and the
// characters take the same shader, and most of what follows is wrong on a
// background: a painted backdrop does not want its colours rounded, does not
// want a rim, and has no baked occlusion worth reading.
float4 toonExtra : register(c30);
float4 toonExtra2 : register(c31);

// The glint and the rim's blend, on a third register.
//
// **Declared for the TOON build alone, because c32 is past what ps_2_0 has.**
// outline_PS.hlsl includes this header and is ps_2_0, which stops at c31 and
// would refuse the line below. Everything that reads these is under the same
// guard, and the ink pass has no use for either.
#ifdef TOON
float4 toonExtra3 : register(c32);

#define toonGloss (toonExtra3.x)
#define toonGlossEdge (toonExtra3.y)
#define toonRimBlend (toonExtra3.z)
#endif

#define toonEnabled (toonParams.x)
#define toonSaturation (toonParams.z)
#define toonStrength (toonParams.w)

// **y was the band count and nothing has read it since the bands moved into the
// ramp texture.** Reusing a dead slot rather than reaching for c32: the non-toon
// variants of this shader are ps_2_0 and share this header, and ps_2_0 has 32
// registers.
#define toonModelShade (toonParams.y)

#define toonColors (toonExtra.x)
#define toonRampRow (toonExtra.y)
#define toonIsCharacter (toonExtra.z)
#define toonWrap (toonExtra.w)

#define toonRim (toonExtra2.x)
#define toonRimEdge (toonExtra2.y)
#define toonOcclusion (toonExtra2.z)
#define toonHardness (toonExtra2.w)

// The colour strip the light term looks up. Band count, widths and colours are
// properties of the texture, so a character can be retuned without a rebuild.
RW_TEXTURE(tex3, 3);

// How many ramps are stacked in the strip. iToon.cpp builds them and the game
// names a row per draw -- skin does not band like sheet metal.
#define TOON_RAMP_ROWS 4.0

float3 ToonRampAt(float l)
{
	// Half a texel in on both axes, so the two ends of a row sample their own
	// colour rather than blending with the clamp, and a row samples itself
	// rather than the one above it.
	return RW_SAMPLE(tex3, float2(clamp(l, 0.02, 0.98),
	                          (toonRampRow + 0.5)/TOON_RAMP_ROWS)).rgb;
}

// The ramp, sampled across one pixel of the light term rather than at a point.
//
// **A band edge is a step function and a step function aliases.** The strip is
// point sampled on purpose -- filtering it would turn every band back into the
// gradient the bands exist to replace -- so the edge is genuinely hard, and at
// one sample per pixel it crawls along a character as he walks.
//
// ddx and ddy say how much the light term changes between this pixel and the
// next, so four taps spread across that are four samples of what the pixel
// actually covers: flat, they land in one band and nothing changes; across an
// edge, they straddle it and the average is the coverage.
//
// **This is why the toon shaders are ps_3_0 and the rest are ps_2_0.** ps_2_0
// has no derivative instructions at all. See make_shaders.cmd.
float3 ToonRamp(float l)
{
	float w = min(abs(ddx(l)) + abs(ddy(l)), 0.08);

	return 0.25*(ToonRampAt(l - 0.375*w) + ToonRampAt(l - 0.125*w) +
	             ToonRampAt(l + 0.125*w) + ToonRampAt(l + 0.375*w));
}

// How much light the level's own models leave standing here.
//
// **A shadow the light rig cannot know about.** The world is lit from a
// direction and a colour, and a house standing in the way is neither -- so the
// scene traces what its placed models block and hands the answer over per
// vertex, in the prelight, which on this path carries nothing else: the vertex
// shader adds no lighting when the pixel shader is going to.
//
// It scales the term BEFORE the ramp rather than darkening the colour after, so
// a shadow crosses the same bands the shading does and reads as part of the same
// drawing.
float ToonModelShade(float3 prelit)
{
	if(toonModelShade <= 0.0)
		return 1.0;

	float v = max(prelit.r, max(prelit.g, prelit.b));

	return lerp(1.0, v, toonModelShade);
}

// What the ramp is indexed by: the facing, wrapped and occluded.
//
// D3D9 has no shadow map yet, so unlike the GL3 side there is nothing else to
// fold in here. When there is, it multiplies in alongside the occlusion and for
// the same reason -- see ToonLight in header.frag.
float ToonLight(float3 N, float3 L, float occlusion)
{
	float ndl = dot(N, -L);

	// Wrapped, if asked. A plain max collapses the whole far hemisphere to one
	// value; the half-lambert remap spreads it over the dark end of the strip
	// and gives it somewhere to put a second tone.
	float l = lerp(max(0.0, ndl), 0.5 + 0.5*ndl, toonWrap);

	return saturate(l*occlusion);
}

// How much light the artists said reaches this vertex, as a scale on the term.
//
// The models carry a baked colour per vertex, and under a chin or inside a fold
// it is dark -- occlusion somebody drew, which no light rig recovers. Geometry
// with no baked colour reads as black, and black here would black the model
// out, so nothing is what an absent value means.
float ToonOcclusion(float3 prelit)
{
	float v = max(prelit.r, max(prelit.g, prelit.b));

	if(toonOcclusion <= 0.0 || toonIsCharacter == 0.0 || v < 1.0/255.0)
		return 1.0;

	return lerp(1.0, v, toonOcclusion);
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
//
// **A panelled prop takes a third of it, spread wide.** A rim traces a
// silhouette by watching the facing turn as the surface curves. It does not turn
// across a flat panel: f is nearly constant, its derivatives are nearly zero,
// and the smoothstep that is meant to soften one pixel becomes a step -- so a
// whole face of a tiki flipped to a quarter of the room's colour at once, with a
// hard line across it, which is the gloss the tikis had. Holding the edge open
// turns that line into a falloff across the face, and the fraction keeps what is
// left quiet. The ramp row is what says a model is panels; iToon.cpp measures it.
float ToonRimAmount(float3 N, float3 V)
{
	if(toonRim <= 0.0 || toonIsCharacter == 0.0)
		return 0.0;

	float ndv = dot(N, normalize(V));

	// **Nothing at all on a face that points away from the eye.** The game
	// leaves culling off unless a model's pipe flags ask for it, so a face
	// pointing away is drawn as often as not, and such a face has no silhouette
	// for the eye to see. The step at zero is not a seam: zero facing IS the
	// silhouette, and what lies past it is either nothing or the inside of the
	// model.
	if(ndv <= 0.0)
		return 0.0;

	float f = 1.0 - ndv;

	// Capped like ToonRamp's, and for the same reason.
	float w = clamp(abs(ddx(f)) + abs(ddy(f)), 1.0/255.0, 0.05);

	return toonRim*smoothstep(toonRimEdge - w, toonRimEdge + w, f);
}

// The face's own normal, from how the surface moves across the triangle.
//
// **Welding took the hard edges out of the shading and this puts them back.**
// The hull needs one normal per position or the inflated copy cracks open at
// every corner, so iToon.cpp averages them -- and that same average is what
// lights the surface, rounding off exactly the corners a drawing wants square.
//
// V is the vector to the eye, so its derivatives are the surface's negated, and
// a cross product of two negated vectors is unchanged. The sign is settled
// against the vertex normal either way, because a derivative follows the
// winding and a normal does not.
float3 ToonHardNormal(float3 N, float3 V)
{
	if(toonHardness <= 0.0 || toonIsCharacter == 0.0)
		return N;

	float3 Ng = cross(ddx(V), ddy(V));
	float len2 = dot(Ng, Ng);

	if(len2 < 1e-20)
		return N;

	Ng *= rsqrt(len2);

	if(dot(Ng, N) < 0.0)
		Ng = -Ng;

	return normalize(lerp(N, Ng, toonHardness));
}

// How strongly light bounces off this pixel, as an amount to blend by.
//
// **A band and not a lobe.** A specular highlight is a gradient, and a gradient
// is the thing the cel look exists to remove -- so the half vector's facing is
// thresholded the way the ramp thresholds the light term, one step wide,
// antialiased across the pixel by its own derivatives. What that draws is a
// glint with an edge on it, which is how the show draws light on water.
//
// Not gated on toonIsCharacter. The surfaces that want this are not characters:
// the goo is the one that asks so far, where it is most of what says the surface
// is liquid and not a painted floor.
//
// L is where the light travels, so the direction towards it is its negative.
#ifdef TOON
float ToonGlossAmount(float3 N, float3 V, float3 L)
{
	if(toonGloss <= 0.0)
		return 0.0;

	float3 H = normalize(normalize(V) - normalize(L));
	float s = dot(N, H);

	if(s <= 0.0)
		return 0.0;

	// Capped like ToonRamp's and ToonRimAmount's, and for the same reason.
	float w = clamp(abs(ddx(s)) + abs(ddy(s)), 1.0/255.0, 0.05);

	return toonGloss*smoothstep(toonGlossEdge - w, toonGlossEdge + w, s);
}
#endif

// Cut a colour down to a handful of shades, keeping the colour.
//
// The brightest channel is rounded to a level and the whole colour scaled by
// that ratio, so all three move together and only brightness is stepped.
// Rounding each channel on its own shifts the hue as a colour lands, which is
// banding that draws attention to itself -- see header.frag on the GL3 side.
float3 ToonQuantize(float3 c)
{
	if(toonColors < 2.0)
		return c;

	float v = max(c.r, max(c.g, c.b));

	if(v < 1.0/255.0)
		return c;

	return c*(floor(v*toonColors + 0.5)/toonColors)/v;
}

// Push colour away from grey. Mixing AWAY from luminance -- a factor above one
// -- leaves greys alone and pulls everything else outward.
// How far the colour can be pushed away from grey along one channel before that
// channel leaves 0..1. See header.frag, which says the whole of it.
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
// channel at one shifts the hue of the colours the setting was turned up for.
// Scaling the whole colour down to fit keeps the hue and takes the brightness
// with it, so the levels go dim precisely where they were most colourful.
//
// The luminance is held and the offset from it grown until the first channel
// reaches an end of the range: full setting where there is room, as much as it
// can take where there is not, and the same brightness either way.
float3 ToonSaturate(float3 c)
{
	float l = dot(c, float3(0.299, 0.587, 0.114));
	float3 d = c - l.xxx;

	float g = min(toonSaturation,
	              min(ToonGainLimit(l, d.r),
	                  min(ToonGainLimit(l, d.g), ToonGainLimit(l, d.b))));

	return saturate(l.xxx + max(g, 0.0)*d);
}
