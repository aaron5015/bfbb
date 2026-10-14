// TU-private definitions preserve the retail xCamera inline section.

SHARED_INLINE xVec3& xVec3::invert()
{
    this->x = -this->x;
    this->y = -this->y;
    this->z = -this->z;

    return *this;
}

#if !defined(PS2)
SHARED_INLINE F32 xexp(F32 x)
{
    return std::expf(x);
}
#endif

// MSL declares this in <cmath> and leaves the body to whoever needs it, so
// the GameCube build supplies it here. A host already has it, and namespace
// std is not ours to add to -- compat/cmath brings the global one in instead.
#if defined(__MWERKS__) && !defined(INLINE) && !defined(PS2)
inline float std::expf(float x)
{
    return (float)exp((double)x);
}
#endif

#if !defined(PS2)
SHARED_INLINE F32 xrmod(F32 ang)
{
    F32 frac = 0.15915494f * ang;

    if (frac < 0.0f)
    {
        return (frac - std::ceilf(frac) + 1.0f) * 6.2831855f;
    }
    else if (frac >= 1.0f)
    {
        return (frac - std::floorf(frac)) * 6.2831855f;
    }

    return ang;
}
#endif

#if !defined(PS2)
SHARED_INLINE xVec3& xVec3::operator/=(F32 f)
{
    F32 f2 = 1.0f / f;

    this->x *= f2;
    this->y *= f2;
    this->z *= f2;

    return *this;
}
#endif

SHARED_INLINE xVec3& xVec3::right_normalize()
{
    return this->safe_normalize(xVec3::m_UnitAxisX);
}

#if !defined(PS2)
SHARED_INLINE xVec3& xVec3::safe_normalize(const xVec3& val)
{
    F32 len = this->length2();

    if (len < 0.000099999997f)
    {
        return (*this = val);
    }
    else
    {
        return (*this *= 1.0f / xsqrt(len));
    }
}
#endif

#if !defined(XBOX)
template <>
SHARED_INLINE F32 range_limit<F32>(F32 v, F32 minv, F32 maxv)
{
    if (v <= minv)
    {
        return minv;
    }

    if (v >= maxv)
    {
        return maxv;
    }

    return v;
}
#endif
