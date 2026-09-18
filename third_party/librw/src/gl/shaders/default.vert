VSIN(ATTRIB_POS)	vec3 in_pos;

#ifdef UVXFORM
// The texture coordinate transform, as the two rows of a 2x4 matrix that
// multiplies (u, v, 1, 1). rwrender.h says what the two constant columns mean
// and why there are two of them.
uniform vec4 u_uvXform[2];
#endif

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
// World space, and NOT normalized: interpolating two unit normals across a
// triangle does not give a unit normal, which is why simple.frag normalizes it
// again. skin.vert declares the same output for the same fragment shader.
VSOUT vec3 v_normal;
// From the surface towards the eye, world space. The rim light and the hard
// normal both want it, and neither can be worked out from v_normal alone.
VSOUT vec3 v_viewDir;
#elif !defined(OUTLINE)
// How squarely this vertex faces the light, for the shadow test. Only where
// there is no normal going across anyway -- the per-pixel build works the same
// number out from v_normal, and more accurately. The OUTLINE build declares it
// above instead, for the hull's own shadow.
VSOUT float v_shadowNdl;
#endif

void
main(void)
{
	vec4 Vertex = u_world * vec4(in_pos, 1.0);
	vec3 Normal = mat3(u_normal) * in_normal;

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

	// **The hull's own normal, which is not the one that lights the surface.** It
	// is every normal at this position averaged, so the inflated copy does not
	// crack at a hard corner, and it describes a crease rather than either face
	// meeting there -- iToon.cpp puts it in two texture coordinate sets rather
	// than over the top of the artists'. A model that has not been through
	// iToonHullNormals reads zero here and falls back to the surface's, which is
	// the push this always made.
	vec3 hullLocal = vec3(in_tex1, in_tex2.x);

	if(dot(hullLocal, hullLocal) <= 1e-8)
		hullLocal = in_normal;

	Vertex = u_world * vec4(in_pos + normalize(hullLocal)*thickness*u_outlineSign.x, 1.0);

	// The hull's own facing, for the shadow the ink takes. The normal is in
	// hand here and the fragment stage has no other way to get it.
	v_shadowNdl = DoShadowNdl(Normal);

	// Which of the two inks this vertex belongs to, decided here rather than
	// in a second pass over the whole model: a vertex shader can branch, and
	// the earlier GameCube version could not.
	// rgb is the ink, w says how to read it -- see u_outlineFlags.
	v_outline = in_pos.y < u_outlineColor2.a
	          ? vec4(u_outlineColor2.rgb, u_outlineFlags.y)
	          : vec4(u_outlineColor.rgb, u_outlineFlags.x);
#endif

	gl_Position = u_proj * u_view * Vertex;

#ifdef OUTLINE
	// **The hull takes its depth from further out than it stands.** An inflated
	// copy is geometry in space, so wherever the model comes within a hull width
	// of something else the copy is inside it -- an arm's hull crosses into the
	// chest, a prop's into the floor -- and the ink then wins the depth test
	// against a surface that is in front of it. That reads as a patch of black
	// inside the silhouette instead of a line round it.
	//
	// The pixel stays where the hull is and its depth is taken from a point
	// pushed the same way again, u_outlineSign.y widths further out. The ink loses to
	// anything within that margin of the surface. clipBase is the vertex before
	// the push, so the difference between the two clip positions is what one
	// width does to depth, and the sign of it says which way is away from the
	// eye -- a model wound inside out pushes the other way.
	//
	// The margin shrinks to nothing where the push runs across the view, which
	// is the silhouette itself. So the band that IS the outline keeps its own
	// depth and still draws against a wall directly behind it.
	vec2 outlineDZW = gl_Position.zw - clipBase.zw;
	vec2 outlineRef = gl_Position.zw +
	                  u_outlineSign.y*sign(outlineDZW.y)*outlineDZW;

	gl_Position.z = clamp(outlineRef.x/max(outlineRef.y, 1e-4), -1.0, 1.0)*
	                gl_Position.w;
#endif

#ifdef UVXFORM
	vec4 uv = vec4(in_tex0, 1.0, 1.0);
	v_tex0 = vec2(dot(u_uvXform[0], uv), dot(u_uvXform[1], uv));
#else
	v_tex0 = in_tex0;
#endif

	v_color = in_color;
#ifdef PERPIXEL
	// Everything from the ambient term down happens in the fragment shader
	// instead, the clamp and the material colour with it -- they come after the
	// lighting and cannot be split from it. The prelight goes across untouched.
	v_normal = Normal;

	// From the surface towards the eye. The camera's world position is the view
	// matrix undone: minus its rotation transposed applied to its translation.
	// u_view[i] is a COLUMN, so a dot with one of them is a row of the
	// transpose.
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
