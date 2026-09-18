uniform sampler2D tex0;

FSIN vec4 v_color;
FSIN vec2 v_tex0;
FSIN float v_fog;
#ifdef SHADOWRECEIVER
// Only where a vertex shader writes it. im2d.vert and im3d.vert pair with this
// same fragment shader and have no world position to transform, and reading a
// varying the vertex stage never wrote is a link error rather than a warning.
FSIN vec4 v_shadowPos;
#ifndef PERPIXEL
// How squarely the surface faces the light, interpolated. Where PERPIXEL is on
// the normal is here already and this is worked out from that instead.
FSIN float v_shadowNdl;
#endif
#endif
#ifdef PERPIXEL
FSIN vec3 v_normal;
// From the surface towards the eye, world space. The rim light needs it, and
// ToonHardNormal takes its derivatives as a stand-in for the surface's own.
FSIN vec3 v_viewDir;
#endif

void
main(void)
{
	vec4 color = v_color;

	// What colour it is in here, and how far round the silhouette this pixel
	// sits. Both are worked out with the lighting and both are wanted after the
	// texture, so they are held rather than applied where they are found.
	vec3 toonRoom = vec3(1.0);
	float toonRimAmt = 0.0;
	float toonGlossAmt = 0.0;

#ifdef PERPIXEL
	// lighting.frag declares the uniforms both arms below read.
	vec3 N = normalize(v_normal);

	if(toonEnabled != 0.0){
		// **The lighting is replaced, not shaded on top of.**
		//
		// Mixing the room's per-pixel lighting back in was the obvious way to
		// keep a character tied to where he is standing, and it reads wrong:
		// what comes back with it is the smooth falloff the bands exist to
		// remove, so every band has a gradient inside it and the whole thing
		// looks like banding laid over lighting rather than like a drawing.
		//
		// So the room contributes its COLOUR and nothing else. u_toonRoomTint
		// is flat across the model -- how bright and what colour it is in here,
		// with no direction in it -- which is what a cel is: one flat tone for
		// the light side, one for the dark, and the dark one painted to match
		// the background.
		// Where the light travels and what colour it is in here, both
		// resolved by setLights before the draw -- see lighting.frag.
		vec3 L = u_toonLightDir.xyz;
		vec3 room = u_toonRoomTint.rgb;

		toonRoom = room;

		// The shading normal, hardened back towards the face's own where the
		// setting asks. Separate from the one handed to the shadow test, which
		// wants the interpolated normal and wants it unnormalized.
		vec3 Ns = ToonHardNormal(N, v_viewDir);

		// **The shadow is an input to the ramp, not a multiply after it.**
		// See ToonLight in header.frag for why that is the whole difference
		// between a cast shadow that reads as ink and one that reads as a
		// gradient laid over a drawing.
		float sh = 1.0;
#ifdef SHADOWRECEIVER
		sh = ShadowFactorN(v_shadowPos, v_normal);
#endif

		vec3 cel = ToonRamp(ToonLight(Ns, L,
		                              ToonOcclusion(v_color.rgb) *
		                              ToonModelShade(v_color.rgb),
		                              sh));

		// **The room's colour multiplies BOTH bands, not just the dark one.**
		//
		// Tinting only the shadow was an attempt to keep lit surfaces at the
		// artwork's own colour, and it works right up until the room is not
		// white: Rock Bottom is blue because its LIGHT is blue, so a lit
		// surface that ignores the light comes out the same colour there as in
		// daylight and the place stops being blue. A normally lit room sums to
		// about white and multiplying by it changes nothing, which is why the
		// mistake was invisible in the levels it was tuned in.
		//
		// Flat across the model either way -- there is no normal in it -- so
		// this dims and tints without putting back the smooth falloff the
		// bands exist to remove.
		// 0 is the room's light with the cast shadow and no shading at all,
		// 1 the ramp at full depth. Not white at 0: an unshaded surface should
		// still be as bright and as coloured as the room it is in, and it
		// should still be in shadow when something is over it.
		color.rgb = room*mix(vec3(sh), cel, toonStrength);

		// **N and not Ns.** The rim wants the real surface; the hardened
		// normal is for the bands. ToonRimAmount says what that cost.
		toonRimAmt = ToonRimAmount(N, v_viewDir);
		toonGlossAmt = ToonGlossAmount(N, v_viewDir, L);

		// The material's alpha and not its colour: the cel colour is the
		// room's and the texture's. The same as default_PS.hlsl's TOON arm.
		color.a *= u_matColor.a;
	}else{
		color.rgb = v_color.rgb;
		color.rgb += u_ambLight.rgb*surfAmbient;
		color.rgb += DoDynamicLightPP(N)*surfDiffuse;
		color = clamp(color, 0.0, 1.0);
		color *= u_matColor;
	}
#endif

	color *= texture(tex0, vec2(v_tex0.x, 1.0-v_tex0.y));

	// The silhouette light, after the texture.
	//
	// **Three ways to put it on, because a rim is light and not paint.** Towards
	// the room's colour replaces what is there: at full amount the band IS the
	// room -- white in daylight, blue in Rock Bottom -- so whatever the surface
	// was doing stops at its edge. Screening keeps it, brightening by what is
	// left of the range rather than by a fixed amount, and cannot leave the
	// range however bright either side already is. Adding is the brightest and
	// the only one that clips. See the D3D9 twin in default_PS.hlsl.
	vec3 rimLight = toonRimAmt*toonRoom;

	if(toonRimBlend >= 1.5)
		color.rgb = clamp(color.rgb + rimLight, 0.0, 1.0);
	else if(toonRimBlend >= 0.5)
		color.rgb = 1.0 - (1.0 - color.rgb)*(1.0 - rimLight);
	else
		color.rgb = mix(color.rgb, toonRoom, toonRimAmt);

	// The glint, over everything the surface is. Towards white and not towards
	// the room, unlike the rim: a highlight is the light itself, where a rim is
	// the room seen along an edge.
	color.rgb = mix(color.rgb, vec3(1.0), toonGlossAmt);

	// After the material and before the fog. Before the fog because a shadow is
	// a property of the surface and fog is a property of the air in front of it
	// -- darkening a fogged colour would tint the fog itself.
	//
	// Applied to the whole colour here, which is right while the world's
	// lighting is baked into that colour. If the world is ever lit at run time
	// this has to move to multiply the direct term only and never the ambient,
	// or every shadowed surface goes black. docs/SHADOWS.md says so too; this
	// is the one line it is talking about.
#ifdef SHADOWRECEIVER
#ifdef PERPIXEL
	// The normal is already here for the lighting, so the shadow test can use
	// it to skip surfaces that face away from the light. Those are the ones the
	// caster pass recorded, so they would otherwise compare against themselves.
	//
	// v_normal and not N: ShadowFactorN wants it unnormalized so it can tell a
	// missing normal from a real one, and N is already a NaN where there is no
	// normal to normalize.
	//
	// Only where the cel path did not already take it. It feeds the same number
	// into the ramp instead, and applying it twice would square it.
	if(toonEnabled == 0.0)
		color.rgb *= ShadowFactorN(v_shadowPos, v_normal);
#else
	color.rgb *= ShadowFactorV(v_shadowPos, v_shadowNdl);
#endif
#endif

#ifdef PERPIXEL
	// Only where the cel branch above ran. im2d, im3d and the per-vertex
	// programs share this shader and never take the look; D3D9 applies both
	// in its TOON pixel shader alone.
	if(toonEnabled != 0.0){
		color.rgb = ToonSaturate(color.rgb);

		// **Flattening is the last thing that happens to the colour.**
		//
		// It was done straight after the texture, which put two operations after it
		// that both move a colour off the levels it was just rounded to: the shadow
		// multiplied it down by whatever the filter averaged, and the saturation
		// scaled it. The setting asked for twelve shades and the frame buffer got
		// however many those two produced. Fog is the only thing allowed after,
		// because fog is the air and not the surface.
		//
		// A character and not the world -- a painted background does not want its
		// colours rounded.
		if(toonIsCharacter != 0.0)
			color.rgb = ToonQuantize(color.rgb);
	}
#endif

	color.rgb = mix(u_fogColor.rgb, color.rgb, v_fog);
	DoAlphaTest(color.a);
	FRAGCOLOR(color);
}
