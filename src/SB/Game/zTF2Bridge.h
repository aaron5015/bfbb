#ifndef ZTF2BRIDGE_H
#define ZTF2BRIDGE_H

#include <types.h>

// The game half of the BFBB <-> TF2 bridge. Once per frame, reads the player
// out of the game and hands it to the platform layer (iTF2Bridge.h). Does
// nothing unless BFBB_TF2BRIDGE is set. This half runs before the frame's
// update: it takes TF2's intent, which the update then applies.
void zTF2Bridge_Frame();

// The other half of the same job: builds and sends the state packet. Called
// after zSceneUpdate and the player update, so what TF2 receives is this
// frame's positions. Never sent from zTF2Bridge_Frame -- see the comment there.
void zTF2Bridge_SendState();

// When TF2 is running the movement (BRIDGE_INTENT_OWNS_MOVE): put the player
// where TF2 says, and look through TF2's eyes. Both are no-ops otherwise, and
// during cutscenes, flythroughs and any time BFBB has control of the player.
void zTF2Bridge_AfterPlayerUpdate(); // right after the player entity updates
void zTF2Bridge_AfterCameraUpdate(); // right after zCameraUpdate
// True while TF2's camera is driving BFBB and bfbb_player_visible is off, so
// zEntPlayer_Render can skip SpongeBob's model. It is a draw-time check rather
// than globals.player.Visible, because the game sets that flag itself on every
// scene load and on eEventVisible, and it must keep working for cutscenes.
bool zTF2Bridge_HidesPlayerModel();

// True while TF2 owns the player's movement and position this frame -- the same
// condition that makes zTF2Bridge_AfterPlayerUpdate write them. Game code that
// would otherwise freeze the player until a BFBB animation ends asks this first:
// the bridge pins the player in the air, so that animation never starts and the
// control it would have returned never comes back.
bool zTF2Bridge_DrivesPlayer();

void zTF2Bridge_DebugRenderHitscan(); // temporary hitscan ray visualization
void zTF2Bridge_DebugRenderRockets(); // temporary rocket/radius visualization

// Called from zEntPlayer_Damage once a hit is accepted, so the TF2 host spends
// its own health on it. src may be NULL or a non-villain (a hazard or surface).
// A no-op unless the bridge is running.
void zTF2Bridge_ReportPlayerDamage(struct xBase* src, U32 damage);

// Whether src may damage the player now. BFBB gates every hit on one timer
// (globals.player.DamageTimer), which is right for a melee swing and wrong for an
// attack the game re-applies every frame; the bridge keeps a window per
// (source, hit kind) instead. With the bridge off this is the stock timer.
bool zTF2Bridge_PlayerHitAllowed(struct xBase* src);

// Arms that window for the hit that is landing, and returns the value
// globals.player.DamageTimer should be left holding: the stock timer for a normal
// hit, or a short flinch for an attack re-applied every frame (so it cannot block
// the next melee hit for a second and a half). That flinch also throttles BFBB's
// own reaction -- ouch, shine, hit animation, rumble -- which would otherwise
// strobe at thirteen ticks a second.
F32 zTF2Bridge_NotePlayerHit(struct xBase* src);

// The robot behind a damage-dealing particle system. Tubelet's flame and chomper's
// gas hurt the player through a particle command that names no entity at all, which
// the bridge otherwise reads as a hazard: no pain sound, no damage arrow, and no way
// to tell which attack it was. Returns the nearest live tubelet/chomper at pos, or
// NULL when the bridge is off, which leaves the hit exactly as game had it.
struct xVec3;
// 'struct', not 'class': clang's MSVC mangler encodes the tag kind (U vs V) in the
// symbol, so a 'class' forward declaration here and the real 'struct zNPCCommon'
// in zNPCTypeCommon.h produce two different names and the exe fails to link.
struct zNPCCommon;
struct zNPCCommon* zTF2Bridge_ParticleDamageSource(const struct xVec3* pos);

// A hit the funnel refused (cooldown, control-off, inside a teleport box, cheat
// mode). On a real level this is how a hazard the bridge does not collide at all
// reads: nothing happens, and without the line the only symptom is silence. Printed
// only with BFBB_TF2BRIDGE_AUDIT=1, once per source per two seconds, so calling it
// from a refusal in the damage funnel costs nothing in a normal session.
void zTF2Bridge_AuditRefusedHit(struct xBase* src, const char* reason);

// What kind of hit is about to land, told to the bridge by whatever is dealing the
// damage rather than guessed from the attacker's type: the same tubelet has a flame
// (no shove) and a blast (a bomb bot's shove). Cleared after the hit; NULL pos means
// "no source position", which leaves the usual one.
#define BRIDGE_HITKIND_NONE 0
#define BRIDGE_HITKIND_FIRE 1      // a particle hit: burns, never shoves
#define BRIDGE_HITKIND_EXPLOSION 2 // a blast: shoves like a bomb bot
void zTF2Bridge_NoteHitKind(int kind, const struct xVec3* pos);

// One-shot melee knockback override, consumed by the matching robot's native
// knockback goal. Direction is horizontal in BFBB axes; multiplier scales the
// stock launch impulse. Returns nonzero only for the matching target.
int zTF2Bridge_ConsumeKnockbackOverride(void* npc, float* dirX, float* dirY, float* dirZ, float* multiplier);

#endif
