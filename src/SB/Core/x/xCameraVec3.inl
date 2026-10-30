// TU-private definitions preserve the retail xCamera inline section.

SHARED_INLINE F32 xVec3Length(const xVec3* v)
{
    return xsqrt(v->x * v->x + v->y * v->y + v->z * v->z);
}

#if !defined(XBOX) && !defined(PS2)
SHARED_INLINE void xVec3Sub(xVec3* o, const xVec3* a, const xVec3* b)
{
    o->x = a->x - b->x;
    o->y = a->y - b->y;
    o->z = a->z - b->z;
}
#endif

#if !defined(PS2) && !defined(XBOX)
SHARED_INLINE void xVec3Inv(xVec3* o, const xVec3* v)
{
    o->x = -v->x;
    o->y = -v->y;
    o->z = -v->z;
}
#endif

// Reconstruct a stripped reference at the retail implicit-copy boundary.
// The original caller is unknown; this inline caller is not emitted.
inline void __deadstripped_xCamera_quat(xQuat& dest, const xQuat& source)
{
    dest = source;
}

#if !defined(XBOX) && !defined(PS2)
SHARED_INLINE F32 xacos(F32 x)
{
    return std::acosf(x);
}
#endif

// MSL declares this in <cmath> and leaves the body to whoever needs it, so
// the GameCube build supplies it here. A host already has it, and namespace
// std is not ours to add to -- compat/cmath brings the global one in instead.
#if defined(__MWERKS__) && !defined(INLINE) && !defined(XBOX) && !defined(PS2)
inline float std::acosf(float x)
{
    return (float)acos((double)x);
}
#endif

#if !defined(PS2)
SHARED_INLINE void xVec3AddTo(xVec3* o, const xVec3* v)
{
    o->x += v->x;
    o->y += v->y;
    o->z += v->z;
}
#endif
