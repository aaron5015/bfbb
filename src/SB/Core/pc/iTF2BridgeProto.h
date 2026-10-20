#ifndef ITF2BRIDGEPROTO_H
#define ITF2BRIDGEPROTO_H

// Wire format of the BFBB <-> TF2 bridge. PLAIN C++ ON PURPOSE: only <stdint.h>,
// no BFBB or Source types, so the very same file is dropped into the Source SDK
// tree (src/game/shared/tf2bridge/) and both ends are guaranteed to agree.

#include <stdint.h>

#define BRIDGE_PORT_TF2 27500
#define BRIDGE_PORT_BFBB 27501

#define BRIDGE_MAGIC_INTENT 0x32494642u
#define BRIDGE_MAGIC_STATE 0x31534642u
#define BRIDGE_MAGIC_ROCKET_IMPACT 0x31524942u
#define BRIDGE_MAGIC_NPC_HIT 0x31504842u

#define BRIDGE_IN_ATTACK (1 << 0)
#define BRIDGE_IN_JUMP (1 << 1)
#define BRIDGE_IN_DUCK (1 << 2)
#define BRIDGE_IN_USE (1 << 5)
#define BRIDGE_IN_ATTACK2 (1 << 11)
#define BRIDGE_IN_RELOAD (1 << 13)

#define BRIDGE_STATE_GAMEPLAY 0x1u
#define BRIDGE_STATE_CONTROL_OFF 0x2u
#define BRIDGE_STATE_NPC_TARGETS 0x4u
#define BRIDGE_HIT_CRITICAL 0x1u
#define BRIDGE_HIT_ROCKET 0x2u

#define BRIDGE_INTENT_OWNS_MOVE 0x1u

#define BRIDGE_WEAPON_MELEE 0x1u
#define BRIDGE_WEAPON_FIRED 0x2u
// The active weapon fires TF2 rockets. BFBB treats those as real projectiles and
// does not also fire the instant crosshair stand-in when one is launched.
#define BRIDGE_WEAPON_ROCKET 0x4u

#define BRIDGE_MAX_HITSCAN_RAYS 32
#define BRIDGE_MAX_ROCKETS 32
#define BRIDGE_MAX_NPC_TARGETS 32

#pragma pack(push, 1)

struct BridgeRocketImpactPacket
{
    uint32_t magic;
    uint32_t entIndex;
    float x, y, z;
};

// A hit reported by a TF2-side NPC proxy. npcId is the BFBB xBase id.
// Damage values are retained separately so BFBB can tune knockback and later
// health-pool behavior without deriving either from the proxy's health.
struct BridgeNpcHitPacket
{
    uint32_t magic;
    uint32_t seq;
    uint32_t sessionId;
    uint32_t sceneId;
    uint32_t npcId;
    uint32_t damageType;
    uint32_t hitFlags;
    float damage;
    float baseDamage;
    float knockbackMultiplier;
    float dirX, dirY, dirZ;
};

struct BridgeNpcTarget
{
    uint32_t id;
    int32_t type;
    float x, y, z;
};

struct BridgeIntentPacket
{
    uint32_t magic;
    uint32_t seq;
    uint32_t sessionId;
    float forward;
    float side;
    float pitch;
    float yaw;
    uint32_t buttons;
    int32_t tfclass;
    int32_t tfhealth;
    uint32_t weaponflags;
    uint32_t flags;
    float scale;
    // Per-weapon bridge tuning and TF2 base damage estimate.
    float knockbackMultiplier;
    float weaponDamage;
    float px, py, pz;
    float vx, vy, vz;
    float ex, ey, ez;
    uint32_t hitscanCount;
    float hitscanOrigin[3];
    float hitscanDir[BRIDGE_MAX_HITSCAN_RAYS][3];
    float hitscanRange;

    // TF2 rocket state. BFBB performs the authoritative world sweep and
    // explosion; TF2 supplies the rocket's actual radius and base damage.
    uint32_t rocketCount;
    int32_t rocketEntIndex[BRIDGE_MAX_ROCKETS];
    float rocketPos[BRIDGE_MAX_ROCKETS][3];
    float rocketAttackerPos[BRIDGE_MAX_ROCKETS][3];
    float rocketRadius[BRIDGE_MAX_ROCKETS];
    float rocketDamage[BRIDGE_MAX_ROCKETS];
    float rocketKnockbackMultiplier[BRIDGE_MAX_ROCKETS];
    float rocketDebugLifetime;
};

struct BridgeStatePacket
{
    uint32_t magic;
    uint32_t seq;
    uint32_t sessionId;
    float x, y, z;
    float yaw;
    int32_t health;
    uint32_t sceneId;
    uint32_t flags;
    uint32_t npcCount;
    BridgeNpcTarget npcs[BRIDGE_MAX_NPC_TARGETS];
};

#pragma pack(pop)

#endif
