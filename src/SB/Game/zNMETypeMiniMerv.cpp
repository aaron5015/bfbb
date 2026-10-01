#include "zNMETypeMiniMerv.h"

#include "zEntPlayer.h"
#include "zGlobals.h"
#include "zMovePoint.h"
#include "zNPCTypeRobot.h"
#include "zRenderState.h"
#include "xMath.h"
#include <math.h>

static const S32 MINI_MERV_MUZZLE_BONE = 28;

static xFactoryInst* MiniMervGoalCreate(S32 who, RyzMemGrow* grow, void*)
{
    if (who == zNMEMiniMerv::GOAL_ZAP)
    {
        return new (who, grow) zNMEGoalMiniMervZap(who);
    }

    return NULL;
}

static void MiniMervGoalDestroy(xFactoryInst* inst)
{
    delete inst;
}

void zNME_Register_MiniMervGoal()
{
    static bool registered = false;
    if (registered)
    {
        return;
    }

    xFactory* factory = xBehaveMgr_GetSelf()->GetFactory();
    if (factory != NULL)
    {
        factory->RegItemType(zNMEMiniMerv::GOAL_ZAP, MiniMervGoalCreate, MiniMervGoalDestroy);
        registered = true;
    }
}

S32 zNMEGoalMiniMervZap::Process(en_trantype*, F32 dt, void* ctxt, xScene*)
{
    zNMEMiniMerv* npc = (zNMEMiniMerv*)ctxt;
    if (npc != NULL)
    {
        npc->UpdateZap(dt);
    }

    return 0;
}

zNMEMiniMerv::zNMEMiniMerv(S32 myType) : zNMEStandard(myType)
{
    focus_radius = 18.0f;
    danger_radius = 10.0f;
    warning_time = 1.25f;
    cooldown_time = 0.75f;
    zap_timer = 0.0f;
    cooldown_timer = 0.0f;
    warning_active = 0;
    zap_fired = 0;
    zap_visible = 0;
    pad[0] = 0;
    zap_visual_timer = 0.0f;
    muzzle_flash_timer = 0.0f;
    muzzle_flash_frame_timer = 0.0f;
    muzzle_flash_angle = 0.0f;
    muzzle_flash_scale = 1.0f;
    muzzle_flash_frame = 0;
    muzzle_flash_raster = NULL;

    zap_beam.Prepare();
    zap_beam.TextureSet(NPCC_FindRWRaster("fx_solid"));
    zap_beam.RadiusSet(0.16f, 0.16f);
    RwRGBA zap_color = { 80, 220, 255, 255 };
    zap_beam.ColorSet(&zap_color, &zap_color);

    warning_beam.init(8, "Mini Merv warning");
    warning_beam.set_texture("plankton_laser_bolt");
    warning_beam.cfg.radius = 0.08f;
    warning_beam.cfg.length = 12.0f;
    warning_beam.cfg.vel = 40.0f;
    warning_beam.cfg.fade_dist = 12.0f;
    warning_beam.cfg.kill_dist = 12.0f;
    warning_beam.cfg.safe_dist = 0.0f;
    warning_beam.cfg.hit_radius = 0.0f;
    warning_beam.cfg.rand_ang = 0.0f;
    warning_beam.cfg.scar_life = 0.0f;
    warning_beam.cfg.hit_interval = 0;
    warning_beam.cfg.damage = 0.0f;
    warning_beam.cfg.bolt_uv[0] = { 0.0f, 0.0f };
    warning_beam.cfg.bolt_uv[1] = { 1.0f, 1.0f };
    warning_beam.refresh_config();
}

void zNMEMiniMerv::Setup()
{
    zNMEStandard::Setup();

    if (muzzle_flash_raster == NULL)
    {
        muzzle_flash_raster = NPCC_FindRWRaster("fx_solid");
    }

    if (nav_curr == NULL && npcass != NULL && npcass->movepoint != 0)
    {
        nav_curr = zMovePoint_From_xAssetID(npcass->movepoint);
    }

    if (psy_self != NULL)
    {
        psy_self->BrainBegin();
        psy_self->AddGoal(GOAL_ZAP, NULL);
        psy_self->BrainEnd();
        psy_self->GoalSet(GOAL_ZAP, 0);
    }
}

void zNMEMiniMerv::Reset()
{
    zNMEStandard::Reset();
    zap_timer = 0.0f;
    cooldown_timer = 0.0f;
    warning_active = 0;
    zap_fired = 0;
    zap_visible = 0;
    zap_visual_timer = 0.0f;
    muzzle_flash_timer = 0.0f;
    muzzle_flash_frame_timer = 0.0f;
    muzzle_flash_angle = 0.0f;
    muzzle_flash_scale = 1.0f;
    muzzle_flash_frame = 0;
    warning_beam.reset();
}

bool zNMEMiniMerv::TargetInFocusRange() const
{
    const xVec3* npc_pos = xEntGetPos((xEnt*)this);
    const xVec3* plyr_pos = xEntGetPos(&globals.player.ent);

    F32 radius = focus_radius;
    if (nav_curr != NULL && nav_curr->asset != NULL && nav_curr->asset->arenaRadius > 0.0f)
    {
        const xVec3* mvpt_pos = zMovePointGetPos(nav_curr);
        radius = nav_curr->asset->arenaRadius;

        if (xVec3Dist2(mvpt_pos, plyr_pos) > radius * radius)
        {
            return false;
        }
    }
    else
    {
        if (xVec3Dist2(npc_pos, plyr_pos) > radius * radius)
        {
            return false;
        }
    }

    return true;
}

bool zNMEMiniMerv::TargetInDangerRange() const
{
    const xVec3* npc_pos = xEntGetPos((xEnt*)this);
    const xVec3* plyr_pos = xEntGetPos(&globals.player.ent);
    return xVec3Dist2(npc_pos, plyr_pos) <= danger_radius * danger_radius;
}

void zNMEMiniMerv::GetMuzzlePos(xVec3* pos) const
{
    *pos = *xEntGetPos((xEnt*)this);

    // Mat[28] is the evaluated animated slot identified by the Mini Merv
    // animation sweep as the lowest, swaying laser assembly/muzzle.
    // Mat[] positions are model-local, so transform it through the entity frame.
    if (frame != NULL && model != NULL && model->Mat != NULL && model->BoneCount >= MINI_MERV_MUZZLE_BONE)
    {
        const xVec3& local = *(const xVec3*)&model->Mat[MINI_MERV_MUZZLE_BONE].pos;
        *pos = frame->mat.pos;
        pos->x += frame->mat.right.x * local.x + frame->mat.up.x * local.y + frame->mat.at.x * local.z;
        pos->y += frame->mat.right.y * local.x + frame->mat.up.y * local.y + frame->mat.at.y * local.z;
        pos->z += frame->mat.right.z * local.x + frame->mat.up.z * local.y + frame->mat.at.z * local.z;
    }
}

void zNMEMiniMerv::FireWarningBeam()
{
    xVec3 start;
    GetMuzzlePos(&start);

    xVec3 target = *xEntGetPos(&globals.player.ent);
    xVec3 dir;
    xVec3Sub(&dir, &target, &start);

    F32 len2 = xVec3Length2(&dir);
    if (len2 <= 0.0001f)
    {
        return;
    }

    xVec3SMul(&dir, &dir, 1.0f / sqrtf(len2));
    warning_beam.emit(start, dir);
}

void zNMEMiniMerv::FireZap()
{
    zEntPlayer_Damage((xBase*)this, 1);
    zap_fired = 1;
    zap_visible = 1;
    zap_visual_timer = 0.20f;
    muzzle_flash_timer = 0.20f;
    muzzle_flash_frame_timer = 0.0f;
    muzzle_flash_frame = (S32)(xurand() * 4.0f);
    muzzle_flash_angle = (xurand() - 0.5f) * 6.2831853f;
    muzzle_flash_scale = 0.55f;
    cooldown_timer = cooldown_time;
}

void zNMEMiniMerv::UpdateZapBeam()
{
    if (!zap_visible)
    {
        return;
    }

    xVec3 start;
    GetMuzzlePos(&start);

    xVec3 target = *xEntGetPos(&globals.player.ent);
    zap_beam.Render(&start, &target);
}

void zNMEMiniMerv::UpdateMuzzleFlash(F32 dt)
{
    if (muzzle_flash_timer <= 0.0f)
    {
        return;
    }

    muzzle_flash_timer = MAX(0.0f, muzzle_flash_timer - dt);
    muzzle_flash_frame_timer -= dt;

    // The original effect is extremely fast. Treat the four 16x16 atlas
    // cells as individual short-lived images for now rather than assuming
    // they are a conventional 0->1->2->3 animation.
    while (muzzle_flash_frame_timer <= 0.0f)
    {
        muzzle_flash_frame = (S32)(xurand() * 4.0f);
        muzzle_flash_frame_timer += 0.025f;
        muzzle_flash_angle += (xurand() - 0.5f) * 1.4f;
    }

    // Give the image a quick bloom/shrink pulse over the short flash.
    F32 life = 1.0f - muzzle_flash_timer / 0.20f;
    F32 pulse = sinf(life * 3.14159265f);
    muzzle_flash_scale = 0.55f + 0.45f * pulse;
}

void zNMEMiniMerv::RenderMuzzleFlash()
{
    if (muzzle_flash_timer <= 0.0f)
    {
        return;
    }

    // Retry the lookup here in case the texture was not resident when Setup()
    // first ran. This is also how we avoid silently losing the effect during
    // the initial visual test.
    if (muzzle_flash_raster == NULL)
    {
        muzzle_flash_raster = NPCC_FindRWRaster("fx_solid");
    }

    if (muzzle_flash_raster == NULL)
    {
        return;
    }

    xVec3 pos;
    GetMuzzlePos(&pos);

    xMat3x3 mat;
    xVec3 dir_card;
    xVec3 dir_perp;
    xVec3 dir_up;
    xVec3 pos_top;
    xVec3 pos_bot;
    xVec3 pos_lerp;
    xVec3 pos_vtx;
    F32 uv_lo[2];
    F32 uv_hi[2];
    RwRGBA rgba = { 255, 255, 255, 255 };

    xMat3x3LookAt(&mat, &pos, &globals.camera.mat.pos);

    xVec3Add(&dir_card, &mat.at, &mat.right);
    xVec3Normalize(&dir_card, &dir_card);

    xVec3Sub(&dir_perp, &mat.at, &mat.right);
    xVec3Normalize(&dir_perp, &dir_perp);

    xVec3Copy(&dir_up, &mat.up);

    // Keep the first pass deliberately obvious in-game.
    F32 rad = 0.65f * muzzle_flash_scale;
    xVec3Copy(&pos_top, &pos);
    xVec3Copy(&pos_bot, &pos);
    xVec3AddScaled(&pos_top, &dir_up, rad);
    xVec3AddScaled(&pos_bot, &dir_up, -rad);

    const F32 u0 = (muzzle_flash_frame & 1) ? 0.5f : 0.0f;
    const F32 v0 = (muzzle_flash_frame & 2) ? 0.5f : 0.0f;
    const F32 u1 = u0 + 0.5f;
    const F32 v1 = v0 + 0.5f;
    uv_lo[0] = u0;
    uv_lo[1] = v0;
    uv_hi[0] = u1;
    uv_hi[1] = v1;

    RwIm3DVertex vtxbuf[2][14];
    RwIm3DVertex* vtx_horz = vtxbuf[0];
    RwIm3DVertex* vtx_vert = vtxbuf[1];

    for (S32 i = 0; i <= 6; ++i)
    {
        F32 rat = (F32)i / 6.0f;

        pos_lerp.x = pos_top.x + (pos_bot.x - pos_top.x) * rat;
        pos_lerp.y = pos_top.y + (pos_bot.y - pos_top.y) * rat;
        pos_lerp.z = pos_top.z + (pos_bot.z - pos_top.z) * rat;

        F32 v = uv_lo[1] + (uv_hi[1] - uv_lo[1]) * rat;

        pos_vtx = dir_card * rad;
        pos_vtx += pos_lerp;
        RwIm3DVertexSetPos(&vtx_horz[0], pos_vtx.x, pos_vtx.y, pos_vtx.z);
        RwIm3DVertexSetRGBA(&vtx_horz[0], rgba.red, rgba.green, rgba.blue, rgba.alpha);
        RwIm3DVertexSetUV(&vtx_horz[0], uv_lo[0], v);

        pos_vtx = dir_card * -rad;
        pos_vtx += pos_lerp;
        RwIm3DVertexSetPos(&vtx_horz[1], pos_vtx.x, pos_vtx.y, pos_vtx.z);
        RwIm3DVertexSetRGBA(&vtx_horz[1], rgba.red, rgba.green, rgba.blue, rgba.alpha);
        RwIm3DVertexSetUV(&vtx_horz[1], uv_hi[0], v);

        vtx_horz += 2;

        pos_vtx = dir_perp * -rad;
        pos_vtx += pos_lerp;
        RwIm3DVertexSetPos(&vtx_vert[0], pos_vtx.x, pos_vtx.y, pos_vtx.z);
        RwIm3DVertexSetRGBA(&vtx_vert[0], rgba.red, rgba.green, rgba.blue, rgba.alpha);
        RwIm3DVertexSetUV(&vtx_vert[0], uv_lo[0], v);

        pos_vtx = dir_perp * rad;
        pos_vtx += pos_lerp;
        RwIm3DVertexSetPos(&vtx_vert[1], pos_vtx.x, pos_vtx.y, pos_vtx.z);
        RwIm3DVertexSetRGBA(&vtx_vert[1], rgba.red, rgba.green, rgba.blue, rgba.alpha);
        RwIm3DVertexSetUV(&vtx_vert[1], uv_hi[0], v);

        vtx_vert += 2;
    }

    _SDRenderState old_rendstat = zRenderStateCurrent();
    if (old_rendstat == SDRS_Unknown)
    {
        old_rendstat = SDRS_Default;
    }

    zRenderState(SDRS_NPCVisual);
    RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)rwBLENDONE);
    RwRenderStateSet(rwRENDERSTATETEXTURERASTER, (void*)muzzle_flash_raster);

    RwIm3DTransform(vtxbuf[0], 14, NULL,
                    rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA | rwIM3D_VERTEXUV);
    RwIm3DRenderPrimitive(rwPRIMTYPETRISTRIP);
    RwIm3DEnd();

    RwRenderStateSet(rwRENDERSTATETEXTURERASTER, (void*)muzzle_flash_raster);
    RwIm3DTransform(vtxbuf[1], 14, NULL,
                    rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA | rwIM3D_VERTEXUV);
    RwIm3DRenderPrimitive(rwPRIMTYPETRISTRIP);
    RwIm3DEnd();

    zRenderState(old_rendstat);
}

void zNMEMiniMerv::UpdateZap(F32 dt)
{
    if (globals.player.Health < 1)
    {
        warning_beam.reset();
        zap_timer = 0.0f;
        cooldown_timer = 0.0f;
        warning_active = 0;
        zap_fired = 0;
        return;
    }

    if (!TargetInFocusRange())
    {
        warning_beam.reset();
        zap_timer = 0.0f;
        warning_active = 0;
        zap_fired = 0;
        return;
    }

    if (cooldown_timer > 0.0f)
    {
        cooldown_timer = MAX(0.0f, cooldown_timer - dt);
    }

    if (!TargetInDangerRange())
    {
        zap_timer = 0.0f;
        warning_active = 0;
        zap_fired = 0;
        return;
    }

    if (cooldown_timer > 0.0f)
    {
        return;
    }

    if (!warning_active)
    {
        warning_active = 1;
        zap_fired = 0;
        zap_timer = warning_time;
    }

    FireWarningBeam();
    zap_timer -= dt;

    if (zap_timer <= 0.0f && !zap_fired)
    {
        FireZap();
        warning_active = 0;
    }
}

void zNMEMiniMerv::Process(xScene* xscn, F32 dt)
{
    zNMEStandard::Process(xscn, dt);

    if (globals.player.Health > 0 && TargetInFocusRange())
    {
        xVec3 dir;
        xVec3Sub(&dir, xEntGetPos(&globals.player.ent), xEntGetPos((xEnt*)this));
        dir.y = 0.0f;
        if (xVec3Length2(&dir) > 0.0001f)
        {
            xVec3Normalize(&dir, &dir);
            TurnToFace(dt, &dir, 4.0f);
            xEntMotionToMatrix((xEnt*)this, this->frame);
        }
    }

    warning_beam.update(dt);
    UpdateMuzzleFlash(dt);

    if (zap_visual_timer > 0.0f)
    {
        zap_visual_timer = MAX(0.0f, zap_visual_timer - dt);
        if (zap_visual_timer <= 0.0f)
        {
            zap_visible = 0;
        }
    }

    if (warning_beam.visible() || zap_visible)
    {
        flg_xtrarend |= 0x1;
    }
}

void zNMEMiniMerv::RenderExtra()
{
    warning_beam.render();
    UpdateZapBeam();
    RenderMuzzleFlash();
}

void zNMEMiniMerv::SelfDestroy()
{
    warning_beam.reset();
    muzzle_flash_timer = 0.0f;
}

xFactoryInst* ZNME_Create_MiniMerv(S32 who, RyzMemGrow* grow, void*)
{
    if (who == NPC_TYPE_NME_TEST)
    {
        return new (who, grow) zNMEMiniMerv(who);
    }

    return NULL;
}

void ZNME_Destroy_MiniMerv(xFactoryInst* inst)
{
    delete (zNMEMiniMerv*)inst;
}
