const char *skin_matfx_env_vert_src =
"// Three rows per bone; see skin.vert.\n"
"uniform vec4 u_boneMatrices[192];\n"

"vec3 BoneTransform(int bone, vec4 v)\n"
"{\n"
"	return vec3(dot(u_boneMatrices[bone*3], v),\n"
"	            dot(u_boneMatrices[bone*3+1], v),\n"
"	            dot(u_boneMatrices[bone*3+2], v));\n"
"}\n"
"uniform mat4 u_texMatrix;\n"
"uniform vec4 u_colorClamp;\n"
"uniform vec4 u_envColor;\n"

"VSIN(ATTRIB_POS)	vec3 in_pos;\n"

"VSOUT vec4 v_color;\n"
"VSOUT vec4 v_envColor;\n"
"VSOUT vec2 v_tex0;\n"
"VSOUT vec2 v_tex1;\n"
"VSOUT float v_fog;\n"

"void\n"
"main(void)\n"
"{\n"
"	vec3 SkinVertex = vec3(0.0, 0.0, 0.0);\n"
"	vec3 SkinNormal = vec3(0.0, 0.0, 0.0);\n"
"	for(int i = 0; i < 4; i++){\n"
"		SkinVertex += BoneTransform(int(in_indices[i]), vec4(in_pos, 1.0)) * in_weights[i];\n"
"		SkinNormal += BoneTransform(int(in_indices[i]), vec4(in_normal, 0.0)) * in_weights[i];\n"
"	}\n"

"	vec4 Vertex = u_world * vec4(SkinVertex, 1.0);\n"
"	gl_Position = u_proj * u_view * Vertex;\n"
"	vec3 Normal = mat3(u_normal) * SkinNormal;\n"

"	v_tex0 = in_tex0;\n"
"	v_tex1 = (u_texMatrix * vec4(Normal, 1.0)).xy;\n"

"	v_color = in_color;\n"
"	v_color.rgb += u_ambLight.rgb*surfAmbient;\n"
"	v_color.rgb += DoDynamicLight(Vertex.xyz, Normal)*surfDiffuse;\n"
"	v_color = clamp(v_color, 0.0, 1.0);\n"
"	v_envColor = max(v_color, u_colorClamp) * u_envColor;\n"
"	v_color *= u_matColor;\n"

"	v_fog = DoFog(gl_Position.w);\n"
"}\n"
;
