const char *im2d_vert_src =
"uniform vec4 u_xform;\n"

"VSIN(ATTRIB_POS)	vec4 in_pos;\n"

"VSOUT vec4 v_color;\n"
"VSOUT vec2 v_tex0;\n"
"VSOUT float v_fog;\n"

"void\n"
"main(void)\n"
"{\n"
"	gl_Position = in_pos;\n"
"	gl_Position.xy = gl_Position.xy * u_xform.xy + u_xform.zw;\n"
"	v_fog = DoFog(gl_Position.w);\n"
"	gl_Position.xyz *= gl_Position.w;\n"
"#ifdef GL_ES\n"
"	// GLES has no GL_BGRA attribute size, so the bytes arrive in memory order;\n"
"	// see bgraColorAttribs in gl3immed.cpp.\n"
"	v_color = in_color.bgra;\n"
"#else\n"
"	v_color = in_color;\n"
"#endif\n"
"	v_tex0 = in_tex0;\n"
"}\n"
;
