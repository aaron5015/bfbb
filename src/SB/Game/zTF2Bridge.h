#ifndef ZTF2BRIDGE_H
#define ZTF2BRIDGE_H

// The game half of the BFBB <-> TF2 bridge. Once per frame, reads the player
// out of the game and hands it to the platform layer (iTF2Bridge.h). Does
// nothing unless BFBB_TF2BRIDGE is set.
void zTF2Bridge_Frame();

#endif
