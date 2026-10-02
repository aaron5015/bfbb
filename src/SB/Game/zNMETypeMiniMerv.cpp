#include "zNMETypeMiniMerv.h"

// BFBB's global `null` macro collides with librw's namespace null.
// Temporarily remove the macro while taking the librw declarations, then
// restore it for the rest of this game source file.
#ifdef null
#undef null
#define MINI_MERV_RESTORE_NULL
#endif
#include <rw.h>
#ifdef MINI_MERV_RESTORE_NULL
#define null 0
#undef MINI_MERV_RESTORE_NULL
#endif

#include "zEntPlayer.h"
#include "zGlobals.h"
#include "zMovePoint.h"
#include "zNPCTypeRobot.h"
#include "zRenderState.h"
#include "xMath.h"
#include <math.h>

static const S32 MINI_MERV_MUZZLE_BONE = 28;

// TSSM's muzzle effect changes rapidly, with deliberately uneven motion:
 // the texture cells switch quickly, while the flash expands/collapses at
 // different speeds rather than following one gentle scale ramp.
static const F32 MINI_MERV_MUZZLE_FRAME_TIME[4] =
{
    1.0f / 30.0f,
    1.0f / 30.0f,
    2.0f / 30.0f,
    1.0f / 30.0f
};
static const F32 MINI_MERV_MUZZLE_CYCLE_TIME =
    MINI_MERV_MUZZLE_FRAME_TIME[0] +
    MINI_MERV_MUZZLE_FRAME_TIME[1] +
    MINI_MERV_MUZZLE_FRAME_TIME[2] +
    MINI_MERV_MUZZLE_FRAME_TIME[3];
static const F32 MINI_MERV_MUZZLE_ROTATION_RATE = 7.0f;
static const F32 MINI_MERV_MUZZLE_SCALE[4] =
{
    0.45f,
    1.45f,
    0.75f,
    0.35f
};

static RwRaster* MiniMervCreateInvertedRaster(RwRaster* source)
{
    if (source == NULL)
    {
        return NULL;
    }

    // Let RenderWare/librw convert the source texture into a known 32-bit
    // RwImage layout before touching its pixels. The source raster may not
    // itself be a simple 32-bit RGBA buffer.
    RwImage* image = RwImageCreate(source->width, source->height, 32);
    if (image == NULL || RwImageAllocatePixels(image) == NULL ||
        RwImageSetFromRaster(image, source) == NULL)
    {
        if (image != NULL)
        {
            RwImageDestroy(image);
        }
        return NULL;
    }

    // RGB complement: red -> cyan/blue. Alpha is deliberately preserved.
    // Because this is a 32-bit RwImage, the first three bytes are colour
    // channels regardless of their platform-specific channel ordering.
    for (S32 y = 0; y < image->height; ++y)
    {
        RwUInt8* row = image->cpPixels + (size_t)y * image->stride;

        for (S32 x = 0; x < image->width; ++x)
        {
            RwUInt8* pixel = row + x * 4;
            pixel[0] = 255 - pixel[0];
            pixel[1] = 255 - pixel[1];
            pixel[2] = 255 - pixel[2];
        }
    }

    // The public RenderWare C declarations for FindRasterFormat/SetFromImage
    // exist, but this PC port does not currently link implementations for them.
    // Use librw's native conversion here instead of inventing a raster layout.
    rw::Raster* inverted = rw::Raster::createFromImage(reinterpret_cast<rw::Image*>(image));
    RwImageDestroy(image);

    return reinterpret_cast<RwRaster*>(inverted);
}

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
    close_attack_radius = 9.0f;
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
    muzzle_flash_inverted_raster = NULL;

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
        muzzle_flash_raster = NPCC_FindRWRaster("fx_beam_muzzle_flash");
        muzzle_flash_inverted_raster = MiniMervCreateInvertedRaster(muzzle_flash_raster);
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

bool zNMEMiniMerv::TargetInCloseAttackRange() const
{
    const xVec3* npc_pos = xEntGetPos((xEnt*)this);
    const xVec3* plyr_pos = xEntGetPos(&globals.player.ent);
    return xVec3Dist2(npc_pos, plyr_pos) <= close_attack_radius * close_attack_radius;
}

void zNMEMiniMerv::GetMuzzlePos(xVec3* pos) const
{
    *pos = *xEntGetPos((xEnt*)this);

    // Mat[28] is the evaluated animated slot identified by the Mini Merv
    // animation sweep as the lowest, swaying laser assembly/muzzle.
    // Mat[] positions are model-local, so transform it through the entity frame.
    if (frame != NULL && model != NULL && model->Mat != NULL && model->BoneCount >= MINI_MERV_MUZZLE_BONE)
    {
        const xMat4x3& bone = *(const xMat4x3*)&model->Mat[MINI_MERV_MUZZLE_BONE];
        const xVec3 muzzle_offset = { 0.017167f, -0.001860f, 1.038400f };

        // Bone matrices are model-local. First move the tip offset through
        // the animated bone, then transform the result into world space.
        xVec3 local;
        local.x = bone.pos.x + bone.right.x * muzzle_offset.x + bone.up.x * muzzle_offset.y + bone.at.x * muzzle_offset.z;
        local.y = bone.pos.y + bone.right.y * muzzle_offset.x + bone.up.y * muzzle_offset.y + bone.at.y * muzzle_offset.z;
        local.z = bone.pos.z + bone.right.z * muzzle_offset.x + bone.up.z * muzzle_offset.y + bone.at.z * muzzle_offset.z;

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
    muzzle_flash_timer = MINI_MERV_MUZZLE_CYCLE_TIME;
    muzzle_flash_frame = (S32)(xurand() * 4.0f);
    muzzle_flash_frame_timer = MINI_MERV_MUZZLE_FRAME_TIME[muzzle_flash_frame];
    muzzle_flash_angle = (xurand() - 0.5f) * 6.2831853f;
    muzzle_flash_scale = MINI_MERV_MUZZLE_SCALE[muzzle_flash_frame];
    cooldown_timer = cooldown_time;
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
        cooldown_timer = 0.0f;
        warning_active = 0;
        zap_fired = 0;
        return;
    }

    const bool close_attack = TargetInCloseAttackRange();

    // Inside the close-attack radius, Mini Merv skips the warning entirely
    // and continuously applies the real close-range zap damage.
    if (close_attack)
    {
        warning_beam.reset();
        warning_active = 0;
        zap_fired = 0;
        cooldown_timer = 0.0f;
        zap_timer = 0.0f;

        FireZap();
        return;
    }

    // Once detected, keep restarting the normal warning -> zap sequence
    // whenever the player remains in danger range. The normal cooldown still
    // separates individual zaps, but it no longer causes the attack to stop.
    if (!TargetInDangerRange())
    {
        warning_beam.reset();
        zap_timer = 0.0f;
        cooldown_timer = 0.0f;
        warning_active = 0;
        zap_fired = 0;
        return;
    }

    if (cooldown_timer > 0.0f)
    {
        cooldown_timer = MAX(0.0f, cooldown_timer - dt);
        if (cooldown_timer > 0.0f)
        {
            FireWarningBeam();
            return;
        }
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

    // The Xbox effect sheet is 64x64 with four 32x32 cells in a 2x2 atlas.
    // TSSM switches these cells much faster than the previous BFBB version.
    // Keep the sequence deterministic after the randomized start.
    while (muzzle_flash_frame_timer <= 0.0f)
    {
        muzzle_flash_frame = (muzzle_flash_frame + 1) & 3;
        muzzle_flash_frame_timer += MINI_MERV_MUZZLE_FRAME_TIME[muzzle_flash_frame];
    }

    // TSSM rotates the billboard continuously rather than choosing a new
    // random angle for every texture frame. The initial angle is randomized
    // per attack, but the motion between frames is consistent.
    muzzle_flash_angle += MINI_MERV_MUZZLE_ROTATION_RATE * dt;

    // Give each transition its own speed. The targets are deliberately
    // exaggerated: TSSM's flash does not gently grow through a uniform ramp.
    const S32 next_frame = (muzzle_flash_frame + 1) & 3;
    const F32 duration = MINI_MERV_MUZZLE_FRAME_TIME[muzzle_flash_frame];
    const F32 frame_t = 1.0f - muzzle_flash_frame_timer / duration;
    const F32 a = MINI_MERV_MUZZLE_SCALE[muzzle_flash_frame];
    const F32 b = MINI_MERV_MUZZLE_SCALE[next_frame];
    muzzle_flash_scale = a + (b - a) * frame_t;
}

void zNMEMiniMerv::RenderMuzzleFlash()
{
    if (muzzle_flash_timer <= 0.0f || muzzle_flash_inverted_raster == NULL)
    {
        return;
    }

    xVec3 pos;
    GetMuzzlePos(&pos);

    xMat3x3 cam_mat;
    xMat3x3LookAt(&cam_mat, &pos, &globals.camera.mat.pos);

    F32 cs = cosf(muzzle_flash_angle);
    F32 sn = sinf(muzzle_flash_angle);

    xVec3 right;
    right.x = cam_mat.right.x * cs + cam_mat.up.x * sn;
    right.y = cam_mat.right.y * cs + cam_mat.up.y * sn;
    right.z = cam_mat.right.z * cs + cam_mat.up.z * sn;

    xVec3 up;
    up.x = -cam_mat.right.x * sn + cam_mat.up.x * cs;
    up.y = -cam_mat.right.y * sn + cam_mat.up.y * cs;
    up.z = -cam_mat.right.z * sn + cam_mat.up.z * cs;

    // Render two crossed camera-facing planes at the same size. This gives the
    // flash some depth from different viewing angles without adding a second
    // scaled copy of the animated texture.
    F32 rad = 0.75f * muzzle_flash_scale;

    xVec3 r = right * rad;
    xVec3 u = up * rad;
    xVec3 d = cam_mat.at * rad;

    xVec3 p0 = pos - r - u;
    xVec3 p1 = pos + r - u;
    xVec3 p2 = pos + r + u;
    xVec3 p3 = pos - r + u;

    xVec3 q0 = pos - d - u;
    xVec3 q1 = pos + d - u;
    xVec3 q2 = pos + d + u;
    xVec3 q3 = pos - d + u;

    // fx_beam_muzzle_flash is a 64x64 Xbox texture with four 32x32 atlas cells.
    // Keep bilinear filtering from sampling across the 32x32 atlas-cell
    // boundaries. The Xbox sheet is 64x64, so half a texel is 1/128.
    const F32 texel = 1.0f / 128.0f;
    const F32 u0 = ((muzzle_flash_frame & 1) ? 0.5f : 0.0f) + texel;
    const F32 v0 = ((muzzle_flash_frame & 2) ? 0.5f : 0.0f) + texel;
    const F32 u1 = ((muzzle_flash_frame & 1) ? 1.0f : 0.5f) - texel;
    const F32 v1 = ((muzzle_flash_frame & 2) ? 1.0f : 0.5f) - texel;

    RwIm3DVertex quad[8];

    RwIm3DVertexSetPos(&quad[0], p0.x, p0.y, p0.z);
    RwIm3DVertexSetPos(&quad[1], p1.x, p1.y, p1.z);
    RwIm3DVertexSetPos(&quad[2], p2.x, p2.y, p2.z);
    RwIm3DVertexSetPos(&quad[3], p3.x, p3.y, p3.z);

    RwIm3DVertexSetPos(&quad[4], q0.x, q0.y, q0.z);
    RwIm3DVertexSetPos(&quad[5], q1.x, q1.y, q1.z);
    RwIm3DVertexSetPos(&quad[6], q2.x, q2.y, q2.z);
    RwIm3DVertexSetPos(&quad[7], q3.x, q3.y, q3.z);

    for (S32 i = 0; i < 8; ++i)
    {
        RwIm3DVertexSetRGBA(&quad[i], 180, 235, 255, 100);
    }

    RwIm3DVertexSetUV(&quad[0], u0, v1);
    RwIm3DVertexSetUV(&quad[1], u1, v1);
    RwIm3DVertexSetUV(&quad[2], u1, v0);
    RwIm3DVertexSetUV(&quad[3], u0, v0);

    RwIm3DVertexSetUV(&quad[4], u0, v1);
    RwIm3DVertexSetUV(&quad[5], u1, v1);
    RwIm3DVertexSetUV(&quad[6], u1, v0);
    RwIm3DVertexSetUV(&quad[7], u0, v0);

    zRenderState(SDRS_NPCVisual);
    RwRenderStateSet(rwRENDERSTATETEXTURERASTER, muzzle_flash_inverted_raster);

    RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)TRUE);
    RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)rwBLENDSRCALPHA);
    RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)rwBLENDONE);

    if (RwIm3DTransform(quad, 8, NULL, rwIM3D_VERTEXUV | rwIM3D_VERTEXRGBA))
    {
        RwImVertexIndex index[12] = {
            0, 1, 3, 1, 2, 3,
            4, 5, 7, 5, 6, 7
        };
        RwIm3DRenderIndexedPrimitive(rwPRIMTYPETRILIST, index, 12);
        RwIm3DEnd();
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

    if (muzzle_flash_inverted_raster != NULL)
    {
        RwRasterDestroy(muzzle_flash_inverted_raster);
        muzzle_flash_inverted_raster = NULL;
    }
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
