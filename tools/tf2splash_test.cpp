// Off-target test for src/SB/Game/zTF2Splash.h. No game, no TF2, no assets.
//
//   g++ -std=c++17 -O1 -I src/SB/Game tools/tf2splash_test.cpp -o /tmp/tf2splash_test && /tmp/tf2splash_test
//
// The world is made of analytic shapes (a half-buried boulder, boxes, a ground
// plane at y = 0). Units are BFBB units (+Y up); the rocket radius is TF2's 146
// Source units at bfbb_unit_scale 40.

#include "zTF2Splash.h"

#include <stdio.h>
#include <stdlib.h>
#include <vector>

using namespace tf2splash;

struct Sphere
{
    V3 c;
    float r;
};
struct Box
{
    V3 lo, hi;
};

struct World
{
    std::vector<Sphere> spheres;
    std::vector<Box> boxes;
    bool ground = true; // solid below y = 0
    long rays = 0;
};

static bool SegSphere(const V3& a, const V3& b, const Sphere& s)
{
    const V3 d = Sub(b, a);
    const V3 f = Sub(a, s.c);
    const float A = Dot(d, d);
    const float B = 2.0f * Dot(f, d);
    const float C = Dot(f, f) - s.r * s.r;
    if (A < 1e-12f)
        return C < 0.0f;
    const float disc = B * B - 4.0f * A * C;
    if (disc < 0.0f)
        return false;
    const float sq = sqrtf(disc);
    const float t0 = (-B - sq) / (2.0f * A);
    const float t1 = (-B + sq) / (2.0f * A);
    return (t0 <= 1.0f && t1 >= 0.0f);
}

static bool SegBox(const V3& a, const V3& b, const Box& bx)
{
    float tmin = 0.0f, tmax = 1.0f;
    const float o[3] = { a.x, a.y, a.z };
    const float d[3] = { b.x - a.x, b.y - a.y, b.z - a.z };
    const float lo[3] = { bx.lo.x, bx.lo.y, bx.lo.z };
    const float hi[3] = { bx.hi.x, bx.hi.y, bx.hi.z };
    for (int i = 0; i < 3; ++i)
    {
        if (fabsf(d[i]) < 1e-9f)
        {
            if (o[i] < lo[i] || o[i] > hi[i])
                return false;
        }
        else
        {
            float t0 = (lo[i] - o[i]) / d[i];
            float t1 = (hi[i] - o[i]) / d[i];
            if (t0 > t1)
            {
                const float t = t0;
                t0 = t1;
                t1 = t;
            }
            if (t0 > tmin)
                tmin = t0;
            if (t1 < tmax)
                tmax = t1;
            if (tmin > tmax)
                return false;
        }
    }
    return true;
}

static bool Blocked(void* ctx, const V3& a, const V3& b)
{
    World* w = (World*)ctx;
    w->rays++;
    if (w->ground && (a.y < 0.0f || b.y < 0.0f))
        return true;
    for (const Sphere& s : w->spheres)
        if (SegSphere(a, b, s))
            return true;
    for (const Box& bx : w->boxes)
        if (SegBox(a, b, bx))
            return true;
    return false;
}

// Same body points the game builds from an NPC bound: centre, lower, upper, and
// four around the middle. `h` is the full height of the bound sitting on y = 0
// (or on `floorY`).
static int BodyPoints(float x, float floorY, float h, float halfW, V3* out)
{
    const float cy = floorY + h * 0.5f;
    out[0] = Make(x, cy, 0.0f);
    out[1] = Make(x, floorY + 0.15f * h, 0.0f);
    out[2] = Make(x, floorY + 0.85f * h, 0.0f);
    out[3] = Make(x + 0.4f * halfW, cy, 0.0f);
    out[4] = Make(x - 0.4f * halfW, cy, 0.0f);
    out[5] = Make(x, cy, 0.4f * halfW);
    out[6] = Make(x, cy, -0.4f * halfW);
    return 7;
}

static int gFail = 0;
#define CHECK(cond, ...)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
        {                                                                                          \
            ++gFail;                                                                               \
            printf("  FAIL: ");                                                                    \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

static const float kRadius = 146.0f / 40.0f; // 3.65 BFBB units
static const float kPull = 1.0f / 40.0f; // TF2 pulls the explosion 1 unit off the surface

static float Run(World& w, V3 impact, V3 normal, V3* pts, int n, float wrapScale, bool* wrapped,
                 uint8_t* st)
{
    const V3 origin = Add(impact, Scale(Normalized(normal), kPull));
    Explosion e(origin, normal, kRadius, wrapScale, Blocked, &w);
    const float d = e.Evaluate(pts, n, 3, st);
    if (wrapped)
        *wrapped = e.LastWasWrapped();
    return d;
}

int main()
{
    uint8_t st[16];
    V3 pts[8];
    bool wrapped;

    printf("radius = %.2f BFBB units\n", kRadius);

    // ---- 1. open air ------------------------------------------------------
    {
        printf("[1] open air\n");
        World w;
        w.ground = false;
        const int n = BodyPoints(2.0f, 0.0f, 1.4f, 0.4f, pts);
        const float d = Run(w, Make(0, 0.7f, 0), Make(0, 1, 0), pts, n, 0.0f, &wrapped, st);
        printf("  dist %.2f  falloff %.0f%%\n", d, 100.0f * RocketFalloffScale(d, kRadius));
        CHECK(d > 0.0f && !wrapped, "clear shot must be a straight hit");
        CHECK(fabsf(RocketFalloffScale(0.0f, kRadius) - 1.0f) < 1e-5f, "100%% at the centre");
        CHECK(fabsf(RocketFalloffScale(kRadius, kRadius) - 0.5f) < 1e-5f, "50%% at the edge (TF2 half falloff)");
    }

    // ---- 2. outside the radius -------------------------------------------
    {
        printf("[2] outside radius\n");
        World w;
        w.ground = false;
        const int n = BodyPoints(kRadius + 1.0f, 0.0f, 1.4f, 0.4f, pts);
        const float d = Run(w, Make(0, 0.7f, 0), Make(0, 1, 0), pts, n, 1.6f, &wrapped, st);
        CHECK(d < 0.0f, "beyond the radius must never be hit, wrap or not (got %.2f)", d);
    }

    // ---- 3. tall thick wall must still shield ----------------------------
    {
        printf("[3] tall wall between\n");
        World w;
        w.boxes.push_back({ Make(0.8f, 0.0f, -20.0f), Make(1.4f, 12.0f, 20.0f) });
        const int n = BodyPoints(2.2f, 0.0f, 1.4f, 0.4f, pts);
        const float dPure = Run(w, Make(0, 0.7f, 0), Make(1, 0, 0), pts, n, 0.0f, &wrapped, st);
        const float dWrap = Run(w, Make(0, 0.7f, 0), Make(1, 0, 0), pts, n, 1.6f, &wrapped, st);
        printf("  pure TF2 %.2f   wrap %.2f\n", dPure, dWrap);
        CHECK(dPure < 0.0f, "TF2 rules: wall blocks");
        CHECK(dWrap < 0.0f, "a tall wall must still block even with wrap on");
    }

    // ---- 4. the screenshot: rocket on top of a boulder, robot at its foot -
    {
        printf("[4] boulder: hit on the apex, robot standing beside it\n");
        printf("    robot x   TF2-pure    1-pt centre    7-pt direct    wrap(1.6)\n");
        World w;
        const float rr = 2.0f; // boulder radius, half buried
        w.spheres.push_back({ Make(0, 0, 0), rr });
        const V3 apex = Make(0, rr, 0);
        int rescued = 0, pureHits = 0;
        for (float x = 1.6f; x <= 3.4f; x += 0.2f)
        {
            // Robot stands on the ground at lateral distance x from the boulder axis.
            const int n = BodyPoints(x, 0.0f, 1.4f, 0.4f, pts);
            // 1-point TF2: centre only.
            const float d1 = Run(w, apex, Make(0, 1, 0), pts, 1, 0.0f, &wrapped, st);
            const float d7 = Run(w, apex, Make(0, 1, 0), pts, n, 0.0f, &wrapped, st);
            const float dw = Run(w, apex, Make(0, 1, 0), pts, n, 1.6f, &wrapped, st);
            printf("    %.1f       %-10s  %-10s    %-10s     %s\n", x, d1 >= 0 ? "HIT" : "-",
                   d1 >= 0 ? "HIT" : "-", d7 >= 0 ? "HIT" : "-",
                   dw >= 0 ? (wrapped ? "HIT(wrapped)" : "HIT") : "-");
            if (d7 >= 0)
                ++pureHits;
            if (d7 < 0 && dw >= 0)
                ++rescued;
        }
        CHECK(rescued > 0, "wrap should rescue at least some of the boulder-foot positions");
        printf("    straight-line 7-pt hits: %d   rescued by wrap: %d\n", pureHits, rescued);
    }

    // ---- 5. ledge lip: rocket on the face, robot on top ------------------
    {
        printf("[5] ledge: rocket hits the vertical face, robot is on the top\n");
        World w;
        w.boxes.push_back({ Make(0.0f, -1.0f, -10.0f), Make(8.0f, 3.0f, 10.0f) }); // ledge top y=3
        const V3 hit = Make(0.0f, 2.4f, 0.0f);
        const V3 nrm = Make(-1.0f, 0.0f, 0.0f);
        const int n = BodyPoints(1.2f, 3.0f, 1.4f, 0.4f, pts);
        const float dPure = Run(w, hit, nrm, pts, n, 0.0f, &wrapped, st);
        const float dWrap = Run(w, hit, nrm, pts, n, 1.6f, &wrapped, st);
        printf("  pure TF2: %s   wrap: %s (%.2f)\n", dPure >= 0 ? "HIT" : "blocked",
               dWrap >= 0 ? (wrapped ? "HIT(wrapped)" : "HIT") : "blocked", dWrap);
        CHECK(dWrap >= 0.0f, "robot standing just over the lip should be reachable with wrap");
    }

    // ---- 6. corner peek: centre hidden, head visible ----------------------
    {
        printf("[6] corner peek\n");
        World w;
        w.ground = false;
        // A pillar hides the robot's centre and feet but not its head.
        w.boxes.push_back({ Make(0.9f, -5.0f, -5.0f), Make(1.2f, 0.9f, 5.0f) });
        const int n = BodyPoints(2.3f, 0.0f, 1.8f, 0.4f, pts);
        const float d1 = Run(w, Make(0, 0.9f, 0), Make(1, 0, 0), pts, 1, 0.0f, &wrapped, st);
        const float d7 = Run(w, Make(0, 0.9f, 0), Make(1, 0, 0), pts, n, 0.0f, &wrapped, st);
        printf("  centre-only: %s   7 points: %s\n", d1 >= 0 ? "HIT" : "blocked", d7 >= 0 ? "HIT" : "blocked");
        CHECK(d1 < 0.0f, "centre alone is blocked in this setup");
        CHECK(d7 >= 0.0f, "the exposed head point should still count");
    }

    // ---- 7. cost ----------------------------------------------------------
    {
        printf("[7] ray cost\n");
        World w;
        w.spheres.push_back({ Make(0, 0, 0), 2.0f });
        const int n = BodyPoints(2.3f, 0.0f, 1.4f, 0.4f, pts);
        w.rays = 0;
        Run(w, Make(0, 2.0f, 0), Make(0, 1, 0), pts, n, 1.6f, &wrapped, st);
        printf("  rays for one fully-blocked target with wrap: %ld\n", w.rays);
        CHECK(w.rays < 250, "per-target ray budget blown (%ld)", w.rays);
    }

    printf(gFail ? "\n%d FAILED\n" : "\nall passed\n", gFail);
    return gFail ? 1 : 0;
}
