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

#define BRIDGE_MAGIC_INTENT 0x31494642u // "BFI1"
#define BRIDGE_MAGIC_STATE 0x31534642u // "BFS1"

// Source's IN_* usercmd button bits, copied here so BFBB needs no Source headers.
#define BRIDGE_IN_ATTACK (1 << 0)
#define BRIDGE_IN_JUMP (1 << 1)
#define BRIDGE_IN_DUCK (1 << 2)
#define BRIDGE_IN_USE (1 << 5)
#define BRIDGE_IN_ATTACK2 (1 << 11)
#define BRIDGE_IN_RELOAD (1 << 13)

#define BRIDGE_STATE_GAMEPLAY 0x1u // BFBB is in eGameMode_Game, not a menu/load

#pragma pack(push, 1)

// TF2 -> BFBB: what the TF2 player is asking for this tick.
struct BridgeIntentPacket
{
    uint32_t magic; // BRIDGE_MAGIC_INTENT
    uint32_t seq;
    float forward; // -1..1, from usercmd forwardmove / max speed
    float side; // -1..1, +right
    float pitch; // degrees, view angles
    float yaw; // degrees
    uint32_t buttons; // BRIDGE_IN_*
    int32_t tfclass; // TF_CLASS_* (0 = undefined)
    int32_t tfhealth; // TF2-side health, informational
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
