#ifndef ZTF2BRIDGE_H
#define ZTF2BRIDGE_H

// The game half of the BFBB <-> TF2 bridge. Once per frame, reads the player
// out of the game and hands it to the platform layer (iTF2Bridge.h). Does
// nothing unless BFBB_TF2BRIDGE is set.
void zTF2Bridge_Frame();

// When TF2 is running the movement (BRIDGE_INTENT_OWNS_MOVE): put the player
// where TF2 says, and look through TF2's eyes. Both are no-ops otherwise, and
// during cutscenes, flythroughs and any time BFBB has control of the player.
void zTF2Bridge_AfterPlayerUpdate(); // right after the player entity updates
void zTF2Bridge_AfterCameraUpdate(); // right after zCameraUpdate
void zTF2Bridge_DebugRenderHitscan(); // temporary hitscan ray visualization
void zTF2Bridge_DebugRenderRockets(); // temporary rocket/radius visualization

#endif
