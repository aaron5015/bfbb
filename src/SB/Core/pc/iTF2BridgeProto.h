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
#define BRIDGE_MAGIC_PLAYER_HIT 0x31505042u

#define BRIDGE_IN_ATTACK (1 << 0)
#define BRIDGE_IN_JUMP (1 << 1)
#define BRIDGE_IN_DUCK (1 << 2)
#define BRIDGE_IN_USE (1 << 5)
#define BRIDGE_IN_ATTACK2 (1 << 11)
#define BRIDGE_IN_RELOAD (1 << 13)

#define BRIDGE_STATE_GAMEPLAY 0x1u
#define BRIDGE_STATE_CONTROL_OFF 0x2u
#define BRIDGE_STATE_NPC_TARGETS 0x4u
// The player is in BFBB's play state: alive, in the level, not dying, fading or
// reloading a scene. TF2 uses it to decide when its host may come back, so it must
// not be set while BFBB is still running the death sequence or the checkpoint reload.
#define BRIDGE_STATE_PLAYING 0x8u
// The packet carries where BFBB's own spawn asset is (the PLYR entity, what
// zGameSetupPlayer puts SpongeBob on). TF2 uses it when it has to place its host
// without a live BFBB player to copy -- a TF2 respawn while BFBB is still running
// its death sequence -- because the Valve map's team spawn is usually underneath
// the JSP. Without this flag, spawnX/Y/Z/yaw are zero and must be ignored.
#define BRIDGE_STATE_HAS_SPAWN 0x10u
#define BRIDGE_HIT_CRITICAL 0x1u
#define BRIDGE_HIT_ROCKET 0x2u

#define BRIDGE_INTENT_OWNS_MOVE 0x1u

// TF2 has put its host on BFBB's player for the level it is in, and the position in
// this packet is therefore meaningful. Until it is set, BFBB must not move
// SpongeBob or its camera: a freshly loaded TF2 map places its player at the Valve
// BSP's own spawn point, which is nowhere near the SpongeBob level and often inside
// the map's geometry. Adopting that position is what used to teleport SpongeBob
// under the level on startup, depending on which process finished loading first.
// BFBB's player stays where the level put him until TF2 says it has caught up.
#define BRIDGE_INTENT_PLACED 0x2u

// Draw BFBB's player model while its bridge is looking through TF2's eyes
// (bfbb_player_visible 1). Off by default: the camera sits inside SpongeBob, so the
// model only fills the view with his own geometry. This is visibility only -- the
// collision handover below happens either way, so turning it on cannot change what
// the player can walk into.
#define BRIDGE_INTENT_SHOW_PLAYER 0x4u

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

// One hit on the BFBB player. While the bridge is driving the player, TF2 owns
// the health pool: BFBB still decides that a hit happened (its own damage funnel,
// which already covers robots, hazards, goo and pits), but it no longer spends
// its four chunks of health on it. TF2 scales damage by a convar, so the numbers
// are tunable without touching either engine's damage code.
//
// damage is in BFBB units, where 1 is what a fodder hit used to cost (one chunk.
// A hit that takes the whole remaining pool (a pit, or a hazard that damages for
// the player's current health) is flagged LETHAL and always kills.
#define BRIDGE_PLAYER_HIT_LETHAL 0x1u

// damage is already TF2 health points, sent by BFBB's per-robot table, and must
// not be multiplied by bfbb_player_damage_unit. Without this flag the number is in
// BFBB's chunk units, which is what hazards and untuned robots still send.
#define BRIDGE_PLAYER_HIT_ABSOLUTE 0x2u

// The hit came from a damage-dealing particle system, not from a robot's contact
// damage: tubelet's flame spiral and chomper's gas breath. Fire and gas burn but
// never throw, so TF2 applies the hit (pain sound, damage arrow) without any
// velocity. Set by the particle command itself, so it does not depend on the
// particle-to-robot lookup succeeding.
#define BRIDGE_PLAYER_HIT_NO_SHOVE 0x4u

// The hit is an explosion blast (a tubelet reviving, a duplotron dying). It is
// flagged at the hazard that deals it rather than inferred from the robot type,
// because the same robot can have both a flame and a blast. TF2 gives it the
// explosion shape and a physical damage type regardless of whether the source
// robot was named, and the packet's position is the blast centre, so the shove
// points away from it.
#define BRIDGE_PLAYER_HIT_EXPLOSION 0x8u

// The hit sets the player on fire ("burn" carries how many seconds of exposure it
// represents). BFBB has no afterburn of its own -- its fire only spends health while
// the player is standing in it -- so burning is TF2's, and the flag is what turns it
// on. Set by the tubelet's flame and by monsoon's lightning.
#define BRIDGE_PLAYER_HIT_BURN 0x10u

struct BridgePlayerHitPacket
{
    uint32_t magic;
    uint32_t seq;
    uint32_t sessionId;
    uint32_t sceneId;
    uint32_t npcId;   // BFBB villain id of the source; 0 for a hazard or unknown
    uint32_t flags;   // BRIDGE_PLAYER_HIT_*
    uint32_t npcType; // BFBB en_NPCTYPES of the source ('NTR<' and so on); 0 unknown
    float damage;     // BFBB damage units, or TF2 health when ABSOLUTE is set
    float x, y, z;    // where the hit came from, BFBB world units

    // Which way the attacking robot was facing when the hit landed (its frame's "at"
    // axis, BFBB axes); zero when unknown or for a hazard. A robot that hits from
    // point blank -- a hammer swing or a bomb bot burst inside the player's hull --
    // reports an origin a couple of units from the player, which carries no usable
    // "away from the robot" direction and used to fall back to reversing the
    // player's own movement. That fallback throws a player who is running away from
    // a chaser back into it. The facing does not: it points at the player, so
    // pushing along it is away from the robot in both cases.
    float dirX, dirY, dirZ;

    // Seconds of time-inside-the-fire this hit represents, when BURN is set: one
    // tick of tubelet's flame, or one lightning strike. It is exposure, not a burn
    // duration -- TF2 turns it into its own afterburn, because only TF2 knows how
    // fast afterburn is spent and what its ceiling is.
    float burn;
};

struct BridgeNpcTarget
{
    uint32_t id;
    int32_t type;
    float x, y, z;

    // The robot's own collision bound, relative to (x, y, z): BFBB axes, BFBB
    // units. This is the box the game ray-tests a spatula or a laser against
    // (xBoundGetBox of npc->bound), so TF2's proxy can be the shape the robot
    // actually is instead of one dummy hull for every type. Zeroed when the
    // robot has no usable bound, which leaves the proxy's default size alone.
    float minX, minY, minZ;
    float maxX, maxY, maxZ;

    // The robot's model asset, and the direction it is facing (degrees, same
    // convention as the packet's own yaw). The box above is all BFBB collides
    // with, but it is not the robot: BFBB writes that model's real triangles to
    // bfbb_npcmodels.bfnm (see DumpNpcModel in zTF2Bridge.cpp), keyed by this id
    // and stored in the robot's own local space, and TF2 turns them into the
    // proxy's shape with this yaw. 0 means the model was not exported, and the
    // proxy falls back to the box.
    uint32_t modelId;
    float yaw;
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

    // TF2's own collision hull (Source units, relative to the player's origin).
    // BFBB gives its player this box in place of SpongeBob's model bounds while
    // TF2 runs the movement, so anything in BFBB that collides with the player
    // collides with the hull the merc actually has -- and crouching narrows it
    // in BFBB the same way it does in TF2.
    float hullMins[3];
    float hullMaxs[3];
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
    // BFBB's spawn asset, in BFBB world units and BFBB yaw degrees (see
    // BRIDGE_STATE_HAS_SPAWN). Read from the level's PLYR entity, so it is the
    // spot SpongeBob is placed on rather than wherever he happens to be.
    float spawnX, spawnY, spawnZ;
    float spawnYaw;
    uint32_t npcCount;
    BridgeNpcTarget npcs[BRIDGE_MAX_NPC_TARGETS];
};

#pragma pack(pop)

#endif
