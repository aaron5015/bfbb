uniform mat4 u_boneMatrices[64];

VSIN(ATTRIB_POS)	vec3 in_pos;

VSOUT vec4 v_color;
VSOUT vec2 v_tex0;
VSOUT float v_fog;
// World position in the shadow map's space. Interpolated, then biased into
// texture coordinates by the fragment shader.
VSOUT vec4 v_shadowPos;
#ifdef OUTLINE
// Which ink this vertex is drawn in. Flat across the triangle would be truer to
// a pen, but the regions are split by height and the boundary runs through the
// middle of triangles, so an interpolated colour blends the two over a band of
// a few pixels instead of stepping mid-face.
VSOUT vec4 v_outline;
// How squarely the hull faces the light. outline.frag shadows the ink with it.
VSOUT float v_shadowNdl;
#endif
#ifdef PERPIXEL
// The skinned normal, world space and not normalized. Same output as
// default.vert's; both feed simple.frag's PERPIXEL build.
VSOUT vec3 v_normal;
// And the eye vector, as default.vert declares it.
VSOUT vec3 v_viewDir;
#elif !defined(OUTLINE)
// How squarely this vertex faces the light, for the shadow test. As in
// default.vert, and for the same reason -- and as there, the OUTLINE build
// declares it above instead.
VSOUT float v_shadowNdl;
#endif

void
main(void)
{
	vec3 SkinVertex = vec3(0.0, 0.0, 0.0);
	vec3 SkinNormal = vec3(0.0, 0.0, 0.0);
	for(int i = 0; i < 4; i++){
		SkinVertex += (u_boneMatrices[int(in_indices[i])] * vec4(in_pos, 1.0)).xyz * in_weights[i];
		SkinNormal += (mat3(u_boneMatrices[int(in_indices[i])]) * in_normal) * in_weights[i];
	}

	vec4 Vertex = u_world * vec4(SkinVertex, 1.0);
	vec3 Normal = mat3(u_normal) * SkinNormal;

#ifdef OUTLINE
	// Push the surface out along its own normal before projecting. The normal
	// has to be in hand first, which is why it is computed above the
	// projection here and below it in a stock librw.
	//
	// In model units -- world units on an unscaled atomic, and the same space
	// default_VS.hlsl pushes in -- so the band is thicker up close and thinner
	// far away --
	// which is what a drawn line does NOT do, but scaling by depth instead
	// makes distant characters look inked in marker.
	//
	// **With a floor in screen units, because the alternative is no line.** A
	// fixed world width goes below a pixel somewhere down the level and the
	// character simply stops being inked, which is the one thing an animated
	// drawing never does. u_outlineFlags.z is that floor already divided
	// through by the camera and the render height -- the game works it out,
	// because only the game knows both -- so multiplying by clip w, which is
	// view depth, gives the world width that covers those pixels here.
	vec4 clipBase = u_proj * u_view * Vertex;
	float thickness = max(u_outlineColor.a,
	                      u_outlineFlags.z*max(clipBase.w, 1e-4));


	// **And a ceiling in screen units, because a line that swells is worse.**
	// A fixed world width grows without limit as the camera closes on a
	// character, and a drawing's ink does not: it holds one weight whatever the
	// shot. u_outlineFlags.w is that ceiling, divided through the same way as
	// the floor. Zero means no ceiling.
	if(u_outlineFlags.w > 0.0)
		thickness = min(thickness, u_outlineFlags.w*max(clipBase.w, 1e-4));

	// The hull's own normal, skinned and transformed the same way the surface's
	// was. See default.vert, which says why it is not the surface's.
	vec3 hullLocal = vec3(in_tex1, in_tex2.x);
	vec3 SkinHull = vec3(0.0, 0.0, 0.0);

	if(dot(hullLocal, hullLocal) <= 1e-8)
		hullLocal = in_normal;

	for(int k = 0; k < 4; k++)
		SkinHull += (mat3(u_boneMatrices[int(in_indices[k])]) * hullLocal) * in_weights[k];

	Vertex = u_world * vec4(SkinVertex + normalize(SkinHull)*thickness*u_outlineSign.x, 1.0);

	// The hull's own facing, for the shadow the ink takes. The normal is in
	// hand here and the fragment stage has no other way to get it.
	v_shadowNdl = DoShadowNdl(Normal);

	// Which of the two inks this vertex belongs to, decided here rather than
	// in a second pass over the whole model: a vertex shader can branch, and
	// the earlier GameCube version could not.
	// rgb is the ink, w says how to read it -- see u_outlineFlags.
	// **in_pos, not SkinVertex: the bind pose, not the animated one.**
	//
	// Which ink a vertex belongs to is a fact about the model -- his trousers
	// are his trousers -- and the height it is measured against is worked out
	// once, from the bind pose. Testing the posed position against that
	// threshold moves the boundary every time he lifts a leg.
	v_outline = in_pos.y < u_outlineColor2.a
	          ? vec4(u_outlineColor2.rgb, u_outlineFlags.y)
	          : vec4(u_outlineColor.rgb, u_outlineFlags.x);
#endif

	gl_Position = u_proj * u_view * Vertex;

#ifdef OUTLINE
	// Its depth from a point pushed further out again, so the ink loses to
	// anything the model is nearly touching. See default.vert.
	vec2 outlineDZW = gl_Position.zw - clipBase.zw;
	vec2 outlineRef = gl_Position.zw +
	                  u_outlineSign.y*sign(outlineDZW.y)*outlineDZW;

	gl_Position.z = clamp(outlineRef.x/max(outlineRef.y, 1e-4), -1.0, 1.0)*
	                gl_Position.w;
#endif

	v_tex0 = in_tex0;

	v_color = in_color;
#ifdef PERPIXEL
	// As in default.vert: the lighting moves to the fragment shader whole.
	v_normal = Normal;

	// And the eye vector with it, worked out the same way.
	{
		vec3 t = u_view[3].xyz;
		vec3 camPos = -vec3(dot(u_view[0].xyz, t),
		                    dot(u_view[1].xyz, t),
		                    dot(u_view[2].xyz, t));

		v_viewDir = camPos - Vertex.xyz;
	}
#else
	v_color.rgb += u_ambLight.rgb*surfAmbient;
	v_color.rgb += DoDynamicLight(Vertex.xyz, Normal)*surfDiffuse;
	v_color = clamp(v_color, 0.0, 1.0);
	v_color *= u_matColor;
	v_shadowNdl = DoShadowNdl(Normal);
#endif

	v_shadowPos = u_shadowMatrix * Vertex;

	v_fog = DoFog(gl_Position.w);
}
