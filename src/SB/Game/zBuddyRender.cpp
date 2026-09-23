#include "zBuddyInternal.h"

#include "zGlobals.h"

using namespace zBuddyInternal;

void zBuddy_Render()
{
    if (!enabled || selected == BUDDY_NONE || !globals.camera.lo_cam)
    {
        return;
    }

    const buddy_frame* frame =
        state == BUDDY_STATE_DEAD
            ? &death_frame
            : state == BUDDY_STATE_SKILL
            ? &skill_frames[frame_index]
            : state == BUDDY_STATE_SKILL_RECOVER
            ? &idle_frames[frame_index]
            : state == BUDDY_STATE_STRIKE || state == BUDDY_STATE_RECOVER
            ? &attack_frames[frame_index]
            : state == BUDDY_STATE_CHASE
                ? buddy_moving
                    ? &run_frames[frame_index % (S32)(sizeof(run_frames) / sizeof(run_frames[0]))]
                    : &idle_frames[frame_index % (S32)(sizeof(idle_frames) / sizeof(idle_frames[0]))]
                : buddy_sneaking_sleepy
                    ? &run_frames[frame_index % (S32)(sizeof(run_frames) / sizeof(run_frames[0]))]
                    : follow_running ? &run_frames[frame_index] : &idle_frames[frame_index];
    F32 frame_aspect = (F32)frame->width / (F32)frame->height;
    F32 half_width = buddy_width * frame_aspect * 0.5f;
    F32 half_height = buddy_height * 0.5f;
    F32 u0 = (F32)frame->x / 1024.0f;
    F32 v0 = (F32)frame->y / 512.0f;
    F32 u1 = (F32)(frame->x + frame->width) / 1024.0f;
    F32 v1 = (F32)(frame->y + frame->height) / 512.0f;

    RwMatrix* camera_matrix = RwFrameGetLTM(RwCameraGetFrame(globals.camera.lo_cam));
    RwIm3DVertex quad[4];
    F32 left_x = position.x - camera_matrix->right.x * half_width;
    F32 left_y = position.y - camera_matrix->right.y * half_width;
    F32 left_z = position.z - camera_matrix->right.z * half_width;
    F32 right_x = position.x + camera_matrix->right.x * half_width;
    F32 right_y = position.y + camera_matrix->right.y * half_width;
    F32 right_z = position.z + camera_matrix->right.z * half_width;
    F32 top_x = camera_matrix->up.x * buddy_height;
    F32 top_y = camera_matrix->up.y * buddy_height;
    F32 top_z = camera_matrix->up.z * buddy_height;
    U8 red = buddy_raster != NULL ? 255 : 255;
    U8 green = buddy_raster != NULL ? 255 : 80;
    U8 blue = buddy_raster != NULL ? 255 : 150;
    RwIm3DVertexSetPos(&quad[0], left_x, left_y, left_z);
    U8 alpha = (U8)(255.0f * death_alpha);
    RwIm3DVertexSetRGBA(&quad[0], red, green, blue, alpha);
    RwIm3DVertexSetUV(&quad[0], u0, v1);
    RwIm3DVertexSetPos(&quad[1], left_x + top_x, left_y + top_y, left_z + top_z);
    RwIm3DVertexSetRGBA(&quad[1], red, green, blue, alpha);
    RwIm3DVertexSetUV(&quad[1], u0, v0);
    RwIm3DVertexSetPos(&quad[2], right_x, right_y, right_z);
    RwIm3DVertexSetRGBA(&quad[2], red, green, blue, alpha);
    RwIm3DVertexSetUV(&quad[2], u1, v1);
    RwIm3DVertexSetPos(&quad[3], right_x + top_x, right_y + top_y, right_z + top_z);
    RwIm3DVertexSetRGBA(&quad[3], red, green, blue, alpha);
    RwIm3DVertexSetUV(&quad[3], u1, v0);

    RwRenderStateSet(rwRENDERSTATETEXTURERASTER, buddy_raster);
    RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)rwBLENDSRCALPHA);
    RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)rwBLENDINVSRCALPHA);
    RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)TRUE);
    RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)TRUE);
    RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)FALSE);
    if (RwIm3DTransform(quad, 4, NULL, rwIM3D_VERTEXXYZ | rwIM3D_VERTEXUV |
                                         rwIM3D_VERTEXRGBA) != NULL)
    {
        RwIm3DRenderPrimitive(rwPRIMTYPETRISTRIP);
    }

    if (state != BUDDY_STATE_DEAD)
    {
        F32 skill_ratio = (F32)skill_kills / (F32)skill_kill_cost;
        F32 skill_bar_width = buddy_width * 0.9f;
        F32 skill_bar_height = 0.045f;
        F32 skill_bar_center_y = position.y + buddy_height + 0.20f;
        F32 skill_bar_left_x = position.x - camera_matrix->right.x * skill_bar_width * 0.5f;
        F32 skill_bar_left_y = skill_bar_center_y - camera_matrix->right.y * skill_bar_width * 0.5f;
        F32 skill_bar_left_z = position.z - camera_matrix->right.z * skill_bar_width * 0.5f;
        F32 skill_bar_right_x = position.x + camera_matrix->right.x * skill_bar_width * 0.5f;
        F32 skill_bar_right_y = skill_bar_center_y + camera_matrix->right.y * skill_bar_width * 0.5f;
        F32 skill_bar_right_z = position.z + camera_matrix->right.z * skill_bar_width * 0.5f;
        F32 skill_bar_top_x = camera_matrix->up.x * skill_bar_height * 0.5f;
        F32 skill_bar_top_y = camera_matrix->up.y * skill_bar_height * 0.5f;
        F32 skill_bar_top_z = camera_matrix->up.z * skill_bar_height * 0.5f;
        RwIm3DVertex skill_bar[4];

        RwRenderStateSet(rwRENDERSTATETEXTURERASTER, NULL);
        RwIm3DVertexSetPos(&skill_bar[0], skill_bar_left_x, skill_bar_left_y, skill_bar_left_z);
        RwIm3DVertexSetPos(&skill_bar[1], skill_bar_left_x + skill_bar_top_x,
                           skill_bar_left_y + skill_bar_top_y,
                           skill_bar_left_z + skill_bar_top_z);
        RwIm3DVertexSetPos(&skill_bar[2], skill_bar_right_x, skill_bar_right_y, skill_bar_right_z);
        RwIm3DVertexSetPos(&skill_bar[3], skill_bar_right_x + skill_bar_top_x,
                           skill_bar_right_y + skill_bar_top_y,
                           skill_bar_right_z + skill_bar_top_z);
        for (S32 i = 0; i < 4; i++)
        {
            RwIm3DVertexSetRGBA(&skill_bar[i], 35, 35, 40, 225);
        }
        if (RwIm3DTransform(skill_bar, 4, NULL, rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA) != NULL)
        {
            RwIm3DRenderPrimitive(rwPRIMTYPETRISTRIP);
        }

        F32 skill_fill_width = skill_bar_width * skill_ratio;
        skill_bar_right_x = skill_bar_left_x + camera_matrix->right.x * skill_fill_width;
        skill_bar_right_y = skill_bar_left_y + camera_matrix->right.y * skill_fill_width;
        skill_bar_right_z = skill_bar_left_z + camera_matrix->right.z * skill_fill_width;
        RwIm3DVertexSetPos(&skill_bar[2], skill_bar_right_x, skill_bar_right_y, skill_bar_right_z);
        RwIm3DVertexSetPos(&skill_bar[3], skill_bar_right_x + skill_bar_top_x,
                           skill_bar_right_y + skill_bar_top_y,
                           skill_bar_right_z + skill_bar_top_z);

        U8 skill_left_red = 255;
        U8 skill_left_green = 55;
        U8 skill_left_blue = 55;
        U8 skill_right_red = (U8)(255.0f * (1.0f - skill_ratio));
        U8 skill_right_green = (U8)(55.0f + 200.0f * skill_ratio);
        U8 skill_right_blue = (U8)(55.0f + 200.0f * skill_ratio);
        if (skill_kills >= skill_kill_cost)
        {
            F32 sweep = skill_sweep_timer * 3.0f;
            skill_left_red = (U8)(80.0f + 100.0f * (0.5f + 0.5f * sinf(sweep - 2.1f)));
            skill_left_green = (U8)(150.0f + 105.0f * (0.5f + 0.5f * sinf(sweep)));
            skill_left_blue = (U8)(150.0f + 105.0f * (0.5f + 0.5f * sinf(sweep + 2.1f)));
            skill_right_red = (U8)(80.0f + 100.0f * (0.5f + 0.5f * sinf(sweep + 1.0f)));
            skill_right_green = (U8)(150.0f + 105.0f * (0.5f + 0.5f * sinf(sweep + 3.1f)));
            skill_right_blue = (U8)(150.0f + 105.0f * (0.5f + 0.5f * sinf(sweep + 5.2f)));
        }
        RwIm3DVertexSetRGBA(&skill_bar[0], skill_left_red, skill_left_green, skill_left_blue, 255);
        RwIm3DVertexSetRGBA(&skill_bar[1], skill_left_red, skill_left_green, skill_left_blue, 255);
        RwIm3DVertexSetRGBA(&skill_bar[2], skill_right_red, skill_right_green, skill_right_blue,
                            255);
        RwIm3DVertexSetRGBA(&skill_bar[3], skill_right_red, skill_right_green, skill_right_blue,
                            255);
        if (RwIm3DTransform(skill_bar, 4, NULL, rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA) != NULL)
        {
            RwIm3DRenderPrimitive(rwPRIMTYPETRISTRIP);
        }

        F32 health_ratio = (F32)health / (F32)max_health;
        F32 bar_width = buddy_width * 0.9f;
        F32 bar_height = 0.06f;
        F32 bar_center_y = position.y + buddy_height + 0.12f;
        F32 bar_left_x = position.x - camera_matrix->right.x * bar_width * 0.5f;
        F32 bar_left_y = bar_center_y - camera_matrix->right.y * bar_width * 0.5f;
        F32 bar_left_z = position.z - camera_matrix->right.z * bar_width * 0.5f;
        F32 bar_right_x = position.x + camera_matrix->right.x * bar_width * 0.5f;
        F32 bar_right_y = bar_center_y + camera_matrix->right.y * bar_width * 0.5f;
        F32 bar_right_z = position.z + camera_matrix->right.z * bar_width * 0.5f;
        F32 bar_top_x = camera_matrix->up.x * bar_height * 0.5f;
        F32 bar_top_y = camera_matrix->up.y * bar_height * 0.5f;
        F32 bar_top_z = camera_matrix->up.z * bar_height * 0.5f;
        RwIm3DVertex bar[4];

        RwRenderStateSet(rwRENDERSTATETEXTURERASTER, NULL);
        RwIm3DVertexSetPos(&bar[0], bar_left_x, bar_left_y, bar_left_z);
        RwIm3DVertexSetPos(&bar[1], bar_left_x + bar_top_x, bar_left_y + bar_top_y,
                           bar_left_z + bar_top_z);
        RwIm3DVertexSetPos(&bar[2], bar_right_x, bar_right_y, bar_right_z);
        RwIm3DVertexSetPos(&bar[3], bar_right_x + bar_top_x, bar_right_y + bar_top_y,
                           bar_right_z + bar_top_z);
        for (S32 i = 0; i < 4; i++)
        {
            RwIm3DVertexSetRGBA(&bar[i], 20, 20, 20, 220);
        }
        if (RwIm3DTransform(bar, 4, NULL, rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA) != NULL)
        {
            RwIm3DRenderPrimitive(rwPRIMTYPETRISTRIP);
        }

        F32 fill_width = bar_width * health_ratio;
        bar_right_x = bar_left_x + camera_matrix->right.x * fill_width;
        bar_right_y = bar_left_y + camera_matrix->right.y * fill_width;
        bar_right_z = bar_left_z + camera_matrix->right.z * fill_width;
        RwIm3DVertexSetPos(&bar[2], bar_right_x, bar_right_y, bar_right_z);
        RwIm3DVertexSetPos(&bar[3], bar_right_x + bar_top_x, bar_right_y + bar_top_y,
                           bar_right_z + bar_top_z);
        for (S32 i = 0; i < 4; i++)
        {
            RwIm3DVertexSetRGBA(&bar[i], 60, 220, 80, 255);
        }
        if (RwIm3DTransform(bar, 4, NULL, rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA) != NULL)
        {
            RwIm3DRenderPrimitive(rwPRIMTYPETRISTRIP);
        }
    }
}
