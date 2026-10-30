// TU-private definitions preserve the retail xCamera inline section.

#if !defined(PS2)
SHARED_INLINE void xCameraSetFOV(xCamera* cam, F32 fov)
{
    cam->fov = fov;

    iCameraSetFOV(cam->lo_cam, fov);
}
#endif

SHARED_INLINE F32 xCameraGetFOV(const xCamera* cam)
{
    return cam->fov;
}

SHARED_INLINE void xBinaryCamera::render_debug()
{
}
