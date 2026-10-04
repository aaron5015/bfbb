#ifndef ITF2BRIDGEPROTO_H
#define ITF2BRIDGEPROTO_H

// Wire format of the BFBB <-> TF2 bridge. PLAIN C++ ON PURPOSE: only <stdint.h>,
// no BFBB or Source types, so the very same file is dropped into the Source SDK
// tree (src/game/shared/tf2bridge/) and both ends are guaranteed to agree.
//
// Transport: UDP on 127.0.0.1, one datagram = one packet, little-endian (both
// ends are x86/x64 Windows). Packets are fire-and-forget; a lost one is
// replaced by the next, so every packet carries the full current picture.
//
//   BFBB listens on  27501   (receives BridgeIntentPacket from TF2)
//   TF2  listens on  27500   (receives BridgeStatePacket  from BFBB)

#include <stdint.h>

#define BRIDGE_PORT_TF2 27500
#define BRIDGE_PORT_BFBB 27501

#define BRIDGE_MAGIC_INTENT 0x32494642u // "BFI2"
#define BRIDGE_MAGIC_STATE 0x31534642u // "BFS1"

// Source's IN_* usercmd button bits, copied here so BFBB needs no Source headers.
#define BRIDGE_IN_ATTACK (1 << 0)
#define BRIDGE_IN_JUMP (1 << 1)
#define BRIDGE_IN_DUCK (1 << 2)
#define BRIDGE_IN_USE (1 << 5)
#define BRIDGE_IN_ATTACK2 (1 << 11)
#define BRIDGE_IN_RELOAD (1 << 13)

#define BRIDGE_STATE_GAMEPLAY 0x1u // BFBB is in eGameMode_Game, not a menu/load
#define BRIDGE_STATE_CONTROL_OFF 0x2u // BFBB is moving the player itself (cutscene, flythrough,
                                      // respawn, warp): TF2 must follow BFBB's position

#define BRIDGE_INTENT_OWNS_MOVE 0x1u // TF2 is running the movement: BFBB follows px/py/pz and
                                     // looks through ex/ey/ez. Unset: BFBB walks from forward/side.

// Active weapon classification. Kept deliberately small for the bridge: the BFBB
// side only needs to know which attack model to use, not Source weapon IDs.
#define BRIDGE_WEAPON_MELEE 0x1u
#define BRIDGE_WEAPON_FIRED 0x2u

#define BRIDGE_MAX_HITSCAN_RAYS 32
#define BRIDGE_MAX_ROCKETS 32

// Coordinates. The renderer BFBB uses (librw) flips X when it builds the view
// matrix, so BFBB's world is right-handed with +X toward screen-LEFT, +Y up and
// +Z forward. Source is x forward, y left, z up. That lines up with no mirroring:
//
//      Source = ( bfbb.z, bfbb.x, bfbb.y ) * scale
//      bfbb   = ( source.y, source.z, source.x ) / scale
//
// and yaw is the same number in both (0 = facing BFBB +Z / Source +X, turning
// toward BFBB +X / Source +Y).

#pragma pack(push, 1)

// TF2 -> BFBB: what the TF2 player is asking for this tick.
struct BridgeIntentPacket
{
    uint32_t magic; // BRIDGE_MAGIC_INTENT
    uint32_t seq;
    float forward; // -1..1, from usercmd forwardmove / max speed
    float side; // -1..1, +right
    float pitch; // degrees, view angles (positive looks DOWN, as in Source)
    float yaw; // degrees
    uint32_t buttons; // BRIDGE_IN_*
    int32_t tfclass; // TF_CLASS_* (0 = undefined)
    int32_t tfhealth; // TF2-side health, informational
    uint32_t weaponflags; // BRIDGE_WEAPON_* for the currently active TF2 weapon
    uint32_t flags; // BRIDGE_INTENT_*
    float scale; // Source units per BFBB unit (TF2's bfbb_unit_scale)
    float px, py, pz; // TF2 player origin (feet), Source space
    float vx, vy, vz; // TF2 player velocity, Source space, units/second
    float ex, ey, ez; // TF2 eye position, Source space
    uint32_t hitscanCount; // number of valid TF2-generated hitscan rays in this packet
    float hitscanOrigin[3]; // common shot origin, Source space
    float hitscanDir[BRIDGE_MAX_HITSCAN_RAYS][3]; // exact TF2-generated directions
    float hitscanRange; // Source-unit range for the rays

    // Diagnostic-only TF2 rocket state. BFBB sweeps these positions against its
    // own world collision; no rocket damage or BFBB-side projectile entity exists yet.
    uint32_t rocketCount;
    int32_t rocketEntIndex[BRIDGE_MAX_ROCKETS];
    float rocketPos[BRIDGE_MAX_ROCKETS][3]; // Source-space absolute origin
    float rocketRadius[BRIDGE_MAX_ROCKETS]; // Source-space explosion radius
    float rocketDebugLifetime; // seconds to keep impact/radius diagnostics visible
};

// BFBB -> TF2: where the BFBB player actually is.
struct BridgeStatePacket
{
    uint32_t magic; // BRIDGE_MAGIC_STATE
    uint32_t seq;
    float x, y, z; // BFBB world units
    float yaw; // degrees
    int32_t health; // BFBB player health (underwear count)
    uint32_t sceneId; // BFBB scene id ('HB01' etc as an integer)
    uint32_t flags; // BRIDGE_STATE_*
};

#pragma pack(pop)

#endif
