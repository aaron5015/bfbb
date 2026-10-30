#ifndef ITF2BRIDGE_H
#define ITF2BRIDGE_H

#include <types.h>

#include "iTF2BridgeProto.h"

struct iPadHostState;

// PC-only. The BFBB half of the BFBB <-> TF2 bridge (see iTF2BridgeProto.h).
// Off unless the environment variable BFBB_TF2BRIDGE is set to anything, so a
// normal run is untouched: no socket is opened and every call below is a no-op.
//
// Platform layer, so it knows nothing about the game's types. The game side,
// src/SB/Game/zTF2Bridge.cpp, fills in the state and calls these once a frame.

void iTF2BridgeInit();
void iTF2BridgeExit();
S32 iTF2BridgeActive();

// Drains the socket; the newest intent wins.
void iTF2BridgePoll();

// Sends one state packet, and records whether the game is in play (the
// BRIDGE_STATE_GAMEPLAY flag) for iTF2BridgeApplyPad.
void iTF2BridgeSendState(const BridgeStatePacket* state);
// A hit on the BFBB player, forwarded so the TF2 host can spend its own health
// on it. Fills in magic, seq and session; the caller fills the rest.
void iTF2BridgeSendPlayerHit(const BridgePlayerHitPacket* hit);
void iTF2BridgeSendRocketImpact(uint32_t entIndex, float x, float y, float z);

// Called by the pad backend at the end of its poll on port 0. If the bridge is
// on, BFBB is in gameplay and a fresh intent has arrived, the TF2 player's
// movement and buttons replace the pad's. Otherwise leaves `pad` alone, which
// is what lets a real pad or keyboard still work the BFBB menus.
void iTF2BridgeApplyPad(iPadHostState* pad);

// The game layer works out, from the TF2 view direction and BFBB's camera, which
// stick deflection walks the player the way TF2 is looking, and hands it here.
// Valid for one frame at a time: call every frame, with valid = FALSE to fall
// back to the raw TF2 movement axes.
void iTF2BridgeSetStick(S32 valid, F32 x, F32 y);

// Is something in BFBB waiting for a pad press right now (a talk box)? While TF2
// owns the movement, none of TF2's buttons reach BFBB's pad -- pressing them would
// make SpongeBob jump and attack inside a body TF2 is moving. A talk box is the one
// thing TF2 cannot answer for BFBB, so while this is set the confirm button is
// forwarded anyway. Set every frame by the game layer, FALSE when the bridge is off.
void iTF2BridgeSetPadWanted(S32 wanted);

// Weapon-fire events (a packet carrying hitscan rays or the FIRED flag). Poll
// queues every one, so a shot is not lost when a newer packet arrives in the same
// frame. Returns false when none are waiting; events older than a quarter second
// are discarded.
bool iTF2BridgePopEvent(BridgeIntentPacket* out);
// Proxy damage notifications from TF2, matched to the BFBB scene asset id.
bool iTF2BridgePopNpcHit(BridgeNpcHitPacket* out);

// The newest intent, or NULL when none has arrived in the last quarter second.
const BridgeIntentPacket* iTF2BridgeGetIntent();

#endif
