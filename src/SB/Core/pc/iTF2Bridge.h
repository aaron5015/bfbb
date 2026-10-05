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
void iTF2BridgeSendRocketImpact(uint32_t entIndex, const xVec3* position);

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

// The newest intent, or NULL when none has arrived in the last quarter second.
const BridgeIntentPacket* iTF2BridgeGetIntent();

#endif
