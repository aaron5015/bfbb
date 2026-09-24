uniform vec4 u_xform;

VSIN(ATTRIB_POS)	vec4 in_pos;

VSOUT vec4 v_color;
VSOUT vec2 v_tex0;
VSOUT float v_fog;

void
main(void)
{
	gl_Position = in_pos;
	gl_Position.xy = gl_Position.xy * u_xform.xy + u_xform.zw;
	v_fog = DoFog(gl_Position.w);
	gl_Position.xyz *= gl_Position.w;
#ifdef GL_ES
	// GLES has no GL_BGRA attribute size, so the bytes arrive in memory order;
	// see bgraColorAttribs in gl3immed.cpp.
	v_color = in_color.bgra;
#else
	v_color = in_color;
#endif
	v_tex0 = in_tex0;
}
