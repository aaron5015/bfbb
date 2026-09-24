VSIN(ATTRIB_POS)	vec3 in_pos;

VSOUT vec4 v_color;
VSOUT vec2 v_tex0;
VSOUT float v_fog;

void
main(void)
{
	vec4 Vertex = u_world * vec4(in_pos, 1.0);
	gl_Position = u_proj * u_view * Vertex;
#ifdef GL_ES
	// GLES has no GL_BGRA attribute size, so the bytes arrive in memory order;
	// see bgraColorAttribs in gl3immed.cpp.
	v_color = in_color.bgra;
#else
	v_color = in_color;
#endif
	v_tex0 = in_tex0;
	v_fog = DoFog(gl_Position.w);
}
