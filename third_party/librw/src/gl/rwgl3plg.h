namespace rw {
namespace gl3 {

struct Shader;

void initMatFX(void);
ObjPipeline *makeMatFXPipeline(void);
void matfxRenderCB(Atomic *atomic, InstanceDataHeader *header);
void registerEnvUniforms(void);
void uploadEnvMapState(Texture *envTex, MatFXEnvState *es);

void initSkin(void);
ObjPipeline *makeSkinPipeline(void);
void skinInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance);
void skinRenderCB(Atomic *atomic, InstanceDataHeader *header);
void uploadSkinMatrices(Atomic *atomic);
extern Shader *skinShader, *skinShader_noAT;
extern Shader *skinShader_fullLight, *skinShader_fullLight_noAT;
// Skinning with the lighting left to the fragment shader; pairs with the
// PERPIXEL build of simple.frag, as the default pipeline's does.
extern Shader *skinShader_pp, *skinShader_pp_noAT;
// A skinned caster, for setDepthPassEnabled. Shares depth.frag with the
// unskinned one in gl3device.cpp.
extern Shader *skinDepthShader;
// The caster pass for anything skinned, which the skin and skin+matfx
// pipelines both install as their depthRenderCB.
void skinRenderDepthCB(Atomic *atomic, InstanceDataHeader *header);

ObjPipeline *makeSkinMatFXPipeline(void);
void skinMatfxRenderCB(Atomic *atomic, InstanceDataHeader *header);
void createSkinMatFXShaders(void);
void destroySkinMatFXShaders(void);


}
}
