#ifndef ZTF2SPLASH_H
#define ZTF2SPLASH_H

// TF2-style explosion reach, for the BFBB <-> TF2 bridge.
//
// This header deliberately knows nothing about BFBB or RenderWare: plain floats
// and one callback ("is this straight segment blocked by the level?"). That keeps
// the rules testable off-target; see tools/tf2splash_test.cpp.
//
// What TF2 does (CTFGameRules::RadiusDamage / CTFRadiusDamageInfo::ApplyToEntity,
// src/game/shared/tf/tf_gamerules.cpp):
//   1. the rocket explodes 1 Source unit off the surface it hit (CTFBaseRocket::
//      Explode: endpos + plane.normal * 1);
//   2. every entity whose bounds come within the blast radius is a candidate;
//   3. ONE trace from the explosion to the entity's body target. If it reaches
//      the entity, the entity is hit; if something else gets in the way, it is
//      not. There is no "percent exposed" threshold;
//   4. damage falls off linearly with distance. Rockets carry DMG_HALF_FALLOFF,
//      so it runs from 100% at the explosion down to 50% at the radius edge.
//
// What this adds on top, because BFBB's robots are small and its level is a
// visual mesh with lumpy rocks and thin ledges, not clean TF2 brushwork:
//   * several body points are tried instead of one, so a robot whose centre is
//     clipped by a lip or corner but whose head is clear is still hit;
//   * optionally ("wrap"), when no body point has a straight line, the blast may
//     bend once around the geometry through a waypoint near the explosion. The
//     bent path must stay within wrapScale * radius, and the target must still be
//     inside the radius as the crow flies, so it never extends reach; it only
//     lets a blast flow over a boulder's shoulder or a ledge lip. Set wrapScale
//     to 0 for pure TF2 behaviour.

#include <math.h>
#include <stdint.h>

namespace tf2splash
{
struct V3
{
    float x, y, z;
};

inline V3 Make(float x, float y, float z)
{
    V3 v = { x, y, z };
    return v;
}
inline V3 Add(const V3& a, const V3& b)
{
    return Make(a.x + b.x, a.y + b.y, a.z + b.z);
}
inline V3 Sub(const V3& a, const V3& b)
{
    return Make(a.x - b.x, a.y - b.y, a.z - b.z);
}
inline V3 Scale(const V3& a, float s)
{
    return Make(a.x * s, a.y * s, a.z * s);
}
inline float Dot(const V3& a, const V3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline V3 Cross(const V3& a, const V3& b)
{
    return Make(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
inline float Length(const V3& a)
{
    return sqrtf(Dot(a, a));
}
inline V3 Normalized(const V3& a)
{
    const float len = Length(a);
    return len > 1e-6f ? Scale(a, 1.0f / len) : Make(0.0f, 0.0f, 0.0f);
}

// True when the straight segment from..to is obstructed by level geometry.
typedef bool (*BlockedFn)(void* ctx, const V3& from, const V3& to);

// Per-body-point result, also the colour code of the debug markers.
enum
{
    kStateOutside = 1, // the point lies outside the blast radius
    kStateBlocked = 2, // inside the radius but no usable path
    kStateVisible = 3, // straight line from the explosion
    kStateWrapped = 4 // reached by bending around geometry
};

// TF2: rockets use DMG_HALF_FALLOFF, so damage is 100% at the explosion and 50%
// at the edge of the radius.
inline float RocketFalloffScale(float distance, float radius)
{
    float t = radius > 1e-4f ? distance / radius : 1.0f;
    if (t < 0.0f)
        t = 0.0f;
    if (t > 1.0f)
        t = 1.0f;
    return 1.0f - 0.5f * t;
}

class Explosion
{
public:
    static const int kMaxWaypoints = 64;

    // origin: already pulled off the surface. normal: unit surface normal facing
    // the open side. wrapScale: 0 disables wrapping, otherwise the longest bent
    // path allowed, as a multiple of the radius (values <= 1 also disable it).
    Explosion(const V3& origin, const V3& normal, float radius, float wrapScale,
              BlockedFn blocked, void* ctx)
        : m_origin(origin), m_normal(Normalized(normal)), m_radius(radius),
          m_budget(wrapScale * radius), m_wrap(wrapScale > 1.0f), m_blocked(blocked),
          m_ctx(ctx), m_built(false), m_numWp(0), m_lastWrapped(false)
    {
        if (Length(m_normal) < 0.5f)
            m_normal = Make(0.0f, 1.0f, 0.0f);
    }

    // Tests one target's body points. states[i] receives a k*State value for
    // pts[i]. Only the first wrapCount points are tried for a bent path (the
    // rest are cheap extras for straight lines). Returns the effective distance
    // to use for falloff (straight distance, or bent path length), or a negative
    // number when the blast cannot reach the target at all.
    float Evaluate(const V3* pts, int count, int wrapCount, uint8_t* states)
    {
        m_lastWrapped = false;
        float best = -1.0f;

        for (int i = 0; i < count; ++i)
        {
            const float d = Length(Sub(pts[i], m_origin));
            if (d > m_radius)
            {
                states[i] = kStateOutside;
                continue;
            }
            if (!m_blocked(m_ctx, m_origin, pts[i]))
            {
                states[i] = kStateVisible;
                if (best < 0.0f || d < best)
                    best = d;
            }
            else
            {
                states[i] = kStateBlocked;
            }
        }

        if (best >= 0.0f || !m_wrap)
            return best;

        BuildWaypoints();
        if (m_numWp == 0)
            return best;

        for (int i = 0; i < count && i < wrapCount; ++i)
        {
            if (states[i] != kStateBlocked)
                continue;

            for (int w = 0; w < m_numWp; ++w)
            {
                const float path = m_wpDist[w] + Length(Sub(pts[i], m_wp[w]));
                if (path > m_budget || (best >= 0.0f && path >= best))
                    continue;
                if (m_blocked(m_ctx, m_wp[w], pts[i]))
                    continue;
                best = path;
                states[i] = kStateWrapped;
                m_lastWrapped = true;
            }
        }
        return best;
    }

    bool LastWasWrapped() const
    {
        return m_lastWrapped;
    }

    int WaypointCount() const
    {
        return m_numWp;
    }

private:
    void TryAddWaypoint(const V3& w)
    {
        if (m_numWp >= kMaxWaypoints)
            return;
        const float d = Length(Sub(w, m_origin));
        if (d >= m_budget) // any path through w is at least this long
            return;
        if (m_blocked(m_ctx, m_origin, w))
            return;
        m_wp[m_numWp] = w;
        m_wpDist[m_numWp] = d;
        ++m_numWp;
    }

    // Waypoints sit on a few rings around the explosion in the plane of the
    // surface, lifted off it, plus a few straight out along the normal. They are
    // built once, only when a target needs them, and only kept if the explosion
    // can see them.
    void BuildWaypoints()
    {
        if (m_built)
            return;
        m_built = true;
        m_numWp = 0;

        const V3 axis = fabsf(m_normal.y) < 0.9f ? Make(0.0f, 1.0f, 0.0f) : Make(1.0f, 0.0f, 0.0f);
        const V3 t1 = Normalized(Cross(m_normal, axis));
        const V3 t2 = Cross(m_normal, t1);

        static const float kReach[3] = { 0.30f, 0.60f, 0.90f };
        static const float kLift[2] = { 0.25f, 0.75f };
        const float kTwoPi = 6.2831853f;

        for (int r = 0; r < 3; ++r)
        {
            const float reach = kReach[r] * m_radius;
            TryAddWaypoint(Add(m_origin, Scale(m_normal, reach)));

            for (int l = 0; l < 2; ++l)
            {
                for (int k = 0; k < 8; ++k)
                {
                    const float ang = (kTwoPi / 8.0f) * (float)k;
                    const V3 dir = Add(Scale(t1, cosf(ang)), Scale(t2, sinf(ang)));
                    V3 w = Add(m_origin, Scale(dir, reach));
                    w = Add(w, Scale(m_normal, kLift[l] * reach));
                    TryAddWaypoint(w);
                }
            }
        }
    }

    V3 m_origin;
    V3 m_normal;
    float m_radius;
    float m_budget;
    bool m_wrap;
    BlockedFn m_blocked;
    void* m_ctx;
    bool m_built;
    int m_numWp;
    bool m_lastWrapped;
    V3 m_wp[kMaxWaypoints];
    float m_wpDist[kMaxWaypoints];
};

} // namespace tf2splash

#endif
