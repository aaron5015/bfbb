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

#define BRIDGE_IN_ATTACK (1 << 0)
#define BRIDGE_IN_JUMP (1 << 1)
#define BRIDGE_IN_DUCK (1 << 2)
#define BRIDGE_IN_USE (1 << 5)
#define BRIDGE_IN_ATTACK2 (1 << 11)
#define BRIDGE_IN_RELOAD (1 << 13)

#define BRIDGE_STATE_GAMEPLAY 0x1u
#define BRIDGE_STATE_CONTROL_OFF 0x2u

#define BRIDGE_INTENT_OWNS_MOVE 0x1u

#define BRIDGE_WEAPON_MELEE 0x1u
#define BRIDGE_WEAPON_FIRED 0x2u

#define BRIDGE_MAX_HITSCAN_RAYS 32
#define BRIDGE_MAX_ROCKETS 32

#pragma pack(push, 1)

struct BridgeRocketImpactPacket
{
    uint32_t magic;
    uint32_t entIndex;
    float x, y, z;
};

struct BridgeIntentPacket
{
    uint32_t magic;
    uint32_t seq;
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
    float rocketRadius[BRIDGE_MAX_ROCKETS];
    float rocketDamage[BRIDGE_MAX_ROCKETS];
    float rocketDebugLifetime;
};

struct BridgeStatePacket
{
    uint32_t magic;
    uint32_t seq;
    float x, y, z;
    float yaw;
    int32_t health;
    uint32_t sceneId;
    uint32_t flags;
};

#pragma pack(pop)

#endif