# TF2-in-BFBB Bridge — Roadmap and Current Status

Last updated: 2026-10-25

This document is a project-status supplement, not a replacement for repository history or the existing BFBB buddy/NME project notes. Update it when a milestone is verified or the current task changes.

For the practical sheet — both trees, build commands, the file map, the per-robot damage spec and the last session's changes — see [TF2_BFBB_BRIDGE_HANDOFF.md](TF2_BFBB_BRIDGE_HANDOFF.md).

## Project goal

Make all nine Team Fortress 2 mercenaries playable as the protagonist inside Battle for Bikini Bottom (BFBB), in the spirit of “Super Mario 64 in Minecraft.” BFBB remains the authoritative world and game; TF2 supplies mercenary movement, weapons, class behavior, and relevant player presentation. The goal is to play through BFBB as a mercenary, not to recreate BFBB inside TF2.

## Architecture principles

- **BFBB owns its world and gameplay:** level geometry, world collision, NPCs, native damage/death/rewards, triggers, pickups, checkpoints, warps, progression, and scripted events wherever possible.
- **TF2/Source owns mercenary and weapon behavior:** class selection, movement, weapon firing and attributes, projectile-specific behavior, and TF2 presentation where appropriate.
- **The bridge owns synchronization:** coordinate conversion, player/camera handoff, reliable event delivery, projectile identity/lifecycle, and reconciling TF2 actions with BFBB collision and entities.
- Prefer adapting existing native systems over recreating them in the other engine.
- Keep changes focused and testable; record confirmed behavior separately from assumptions.
- Work only on the tf2-bridge-damage-work branches in aaron5015/bfbb and aaron5015/tf2bfbbbridge. Do not modify aaronshell, TF2 master, or other original branches for this project. Create a safety branch before substantial edits.
- Build both sides when protocol/shared behavior changes could affect both. Do not claim a build or in-game test unless actually performed.

## Current working branches

- BFBB: https://github.com/aaron5015/bfbb/tree/tf2-bridge-damage-work
- TF2 bridge: https://github.com/aaron5015/tf2bfbbbridge/tree/tf2-bridge-damage-work

## Verified foundation (from prior testing)

- The BFBB renderer and world are used; this is not intended to be a TF2 map remake.
- Source-to-BFBB coordinate mapping established as Source = (BFBB.z, BFBB.x, BFBB.y) times the configured scale; inverse mapping is implemented in BFBB.
- bfbb_unit_scale default target is 40 Source units per BFBB world unit.
- TF2 sends position, velocity, eye position, and view angles; BFBB synchronizes SpongeBob/player position and camera to the TF2 view. This is a prototype, not yet the final native player integration.
- During BFBB cutscenes/flythroughs/death/respawn/warps, BFBB may take control and TF2 follows; new-level spawn synchronization is part of the bridge.
- Basic melee and hitscan have been tested; actual TF2 firing events and hitscan spread directions are transmitted to BFBB.
- BFBB hitscan uses native world occlusion so shots do not damage NPCs through JSP world geometry in tested cases.
- Rocket projectiles are tracked in BFBB, swept against BFBB JSP/world collision, terminated on BFBB impact, and show impact/radius diagnostics. User tested a batch of 24 rockets without any passing through collision.
- Grenade Launcher is verified in game: pipes bounce correctly off JSP floor and walls, detonate on direct contact with a proxy, detonate on a bounced contact, and no longer tank the frame rate. Stickies attach to JSP and bounce off proxies without attaching or detonating (same as against a player in TF2).
- Rocket splash damage/exposure has been implemented and iterated. The user chose to keep the current generous wrap-around compromise rather than spend more time revisiting the edge cases. Do not reopen this unless a repeatable gameplay issue warrants it.
- BFBB native NPC damage/death/reward behavior remains the current default model. Damage has been applied during prototype testing; the long-term choice between BFBB-style damage and an optional TF2-style health/damage model is undecided.
- Current testing has focused on NPCs and JSP/static world collision. Do not assume PLAT/SIMP collision has been tested.

## Current task / next milestone

**Playable loop: player damage, death and respawn handoff.** Implemented and validated in game on 2026-10-25: pain sound, flinch, HUD direction arrow and knockback all present, BFBB still owns the death, and TF2 respawns exactly when SpongeBob does.

**Per-robot damage, first pass.** The single-hit numbers are in (table in the handoff); the remaining work is the shape of a hit — tick damage, burn duration, splash falloff — which needs a damage type and duration on the hit packet. Not yet tested in game.

The split is now explicit: BFBB decides that a hit happened (its own `zEntPlayer_Damage` funnel already covers robots, hazards, goo and pits) and TF2 owns the health pool it is spent against. A new `BridgePlayerHitPacket` carries the accepted hit from BFBB to TF2. A hit from a robot in BFBB's table carries TF2 health directly (`BRIDGE_PLAYER_HIT_ABSOLUTE`); hazards and unlisted robots carry BFBB chunks, which `bfbb_player_damage_unit` (default 25) turns into health. A hit flagged lethal (a hazard worth all the health left: a pit, the goo) always kills, and a robot's number never sets that flag — TF2 decides whether it kills. BFBB reports the hit before its own clamp, so lethality survives the conversion.

On the way back, TF2 keeps sending an intent with `tfhealth 0` while its host is dead (written as a literal 0, not `GetHealth()`). BFBB reads that and runs its own death once, via `zEntPlayer_ForceDeathByBridge`, so the fade, rewards, death event, checkpoint and respawn all stay BFBB's.

That forced death does not go through `zEntPlayer_Damage`. The funnel is about hits and refuses to run while cheat mode is on, while the hit cooldown is up, while a control-off (taxi, springboard, bungee, bubble bowl, trampoline) is active, or while the player stands in a teleport box, and it only spends health when `g.TakeDamage` is on. Any of those left TF2 waiting on a player who was still standing until the respawn timeout fired, which is where the occasional wrong respawn came from. The death body is now `zEntPlayer_PlayerDied()` and both paths call it; the bridge path zeroes health, clears the timers and runs it directly, logging once to stdout if a missing model makes it retry.

The respawn itself is now BFBB's call, not TF2's. `BRIDGE_STATE_PLAYING` says the player is back in `eGameState_Play` (alive, in the level, past the death animation, the fade and the checkpoint reload). Once BFBB is seen down, the bridge pushes TF2's respawn timer out of reach with a `SetRespawnOverride` far in the future, which closes both doors: `CTFPlayer::IsReadyToSpawn()` refuses the wave, and `CTFGameRules::CheckRespawnWaves()` only force-respawns a player whose override has run out. When BFBB reports Play again, TF2 is force-respawned in that same tick and re-snapped to BFBB's position, so the two engines can no longer disagree about when the player is back or where. `bfbb_player_respawn_hold` disables the gate, `bfbb_player_respawn_timeout` (12 s) covers BFBB being closed or stuck while TF2 is dead.

Being hit now reads as a hit: robot damage goes in as `DMG_CLUB` instead of `DMG_GENERIC`, which is what enables the pain sound, the flinch and the HUD direction indicator (`CTFPlayer::PainSound` returns early for generic damage, and the `Damage` usermessage only carries a direction for a non-generic type). The hit's reported position is set to the source BFBB sends, and `bfbb_player_knockback` (default 200 u/s horizontal) shoves the host away from the robot. Hazards (npcId 0) stay generic and silent, the way `trigger_hurt` is.

Recognising a robot took a second pass. The bridge first tested `src->baseType == eBaseTypeVillain`, but robots are `zNPCCommon`, and `zNPCCommon::Init` leaves `baseType` as the level asset's own, `eBaseTypeNPC`. Every robot hit therefore arrived with `npcId 0` and was treated as a hazard: no pain sound, no arrow, no shove. Both types are accepted now. `bfbb_bridge_debug 1` prints the hit (npc id, damage, health before/after) and `player hit dropped (alive= control=)` if a guard eats one.

**Next milestone: the first real-level pass.** A fresh save at HB00 played forward to JF04, on real levels rather than `bfbbtest`. Expect it to break where the bridge has no implementation at all — standing on or riding PLATs, trigger volumes, hazards that hurt by collision, warps — and expect no damage from anything that is not a robot NPC. That is the point of the pass: the world-side gap is now the main thing between here and a playable level, and it cannot be closed by guessing which category a given wall belongs to.

The instrument is a **session log** written by both halves into one file (`D:\bfbbpc\bfbb\logs\session_YYYY-MM-DD.log`, on by default, `BFBB_TF2BRIDGE_LOG=0` to disable), plus `bfbb_note <text>` in the TF2 console for the user's own bookmarks. It records the level changes and their load gaps, a 15 s position timeline from both engines, every level entity the first time it comes within reach, every hit BFBB accepted or refused, every shot applied to a robot, deaths and respawns, and where the host was placed. Fixed order of work afterwards: whatever blocks reaching the next warp. Details in the handoff's §5.27 and §10.

Open decisions and caveats:

- BFBB's `g.TakeDamage` must stay 0 while the bridge owns player health. If BFBB also drains its own pool, the player dies twice and the two death flows fight. BFBB's health is only forced to zero when TF2 says the host is dead.
- BFBB's hit cooldown (`g.DamageTimeHit`, sb.ini, default 0.5 s) still throttles how often a robot or hazard can report a hit. Numeric damage replaced the four-chunk model, but not that cooldown; set it to 0 for no damage window at all.
- No ragdoll attribution: bridge damage carries the world entity as attacker because `CTFGameRules::FPlayerCanTakeDamage` rejects a NULL attacker. Kill feed shows an environmental death rather than the robot that hit. The proxy is now found by `npcId` every hit, so using it as attacker is a small, separate change if the feed matters.
- TF2's respawn countdown still shows its own wave (the bridge holds the spawn, it does not rewrite the timer), so `mp_disable_respawn_times 1` keeps the HUD honest. No need to set it for correctness any more.
- Per-robot damage is implemented for the robots with one clear attack: BFBB's `sRobotDamage` table (fodder 25, hammer and ham-spin 50, tartar 40, glove 40, chuck 60, monsoon 45, arf 60, dawgs 30, bomb bot 80, bzzt 40, slick 40) is keyed by `SelfType()` and sent as TF2 health with `BRIDGE_PLAYER_HIT_ABSOLUTE`, so `bfbb_player_damage_unit` no longer applies to those. Hazards and unlisted robots still use the convar.
- Tick damage (tartar puddle, chompbot gas, sleepy beam), burn (tubelet, bomb bot, arf dawgs, monsoon), splash falloff (chuck, slick) and attacks that differ per attack rather than per robot (tubelet's flame against its blast) have no mechanism yet: the hit packet still carries only one amount.
- Knockback scales with the hit: `bfbb_player_knockback` is the shove for a 25-health hit and the impulse is multiplied by `damage / 25`, clamped to 0.6-3.0. There is still no lingering-damage path, so tubelet, bomb bot, arf dawgs and monsoon lose their fire after the first hit.
- No pain levels yet. `CTFPlayer::PainSound` does not branch on how much damage the hit did: it speaks `MP_CONCEPT_PLAYER_PAIN` for anything non-generic and throttles by the length of the response, so a fodder prod and a bomb bot sound the same. Distinct lines would mean picking the concept per hit (or adding the damage to the response context) rather than relying on the existing path.
- Ragdolls do not respect `bfbb_coll_physics 1` and fall through the BFBB JSP; only the living player is collided against it.
- One JSP triangle on a wide flat plane under the goo (walkable in BFBB) is fallen through in TF2 while the surrounding geometry is not. Suspected degenerate or wound-back triangle in the collision builder rather than a coordinate problem; a diagnostic that names the offending triangle would let the whole game be swept for the same thing.
- Checkpoints, pickups and warps are still native BFBB because BFBB's player entity keeps moving with the mercenary. Only damaging and dying moved to TF2.
- Multiplayer: every player instance updating each other's worlds is not started, and resets are per-machine for now.

## Roadmap

### 1. Player-control contract and camera synchronization
- Define player authority for normal play, cutscenes, flythroughs, death, respawn, warps, and level changes.
- Keep TF2 view direction and BFBB camera orientation aligned.
- Ensure BFBB player facing/movement is consistent with the mercenary, rather than treating the BFBB player as a position-only teleported proxy.
- Prevent snapping, stale facing, and incorrect movement axes during handoffs.
- Make player-dependent native BFBB systems recognize the active mercenary consistently; revisit cases such as Sleepy detection that may depend on normal player movement/update flow.

### 2. Reliable end-to-end playable loop
- Player damage, death, the respawn handoff and hit feedback are implemented on both sides; the remaining item is the in-game validation below.
- Spawn, move, jump, take damage, die, and respawn.
- Activate a checkpoint and return to its location after death.
- Collect a pickup and verify BFBB's native progression/reward behavior.
- Trigger a level transition/warp and ensure TF2 follows correctly.
- Use this as a recurring integration test rather than testing only isolated weapons.

### 3. Weapon foundation
- **Done, per the user (2026-10-29):** rocket knockback, and the Grenade Launcher and stickybombs (bouncing, sticking to JSP, arming, detonation). Do not re-list these as pending work.
- **Explosive juggling is gated, not pending.** A rocket cannot juggle a robot until robots have TF2-style health pools (the optional damage-model convar in the handoff's parked list), because BFBB's few-hit-kill model has nothing to juggle against. Expected to wait for real rendering rather than the next combat round.
- Broaden melee/hitscan testing, including spread and weapons that override common fire paths.
- Cover niche projectiles and alternate weapons (e.g. Sandman ball, Guillotine) with weapon-specific behavior.
- Add crit handling and account for TF2 weapon attributes that affect damage, firing, projectile speed, and other behavior.
- Keep BFBB native damage/death as the current model while evaluating a configurable TF2-style damage/health model as an optional future mode. Crits and ordinary hits will differ depending on which model is selected.

### 4. Native BFBB interactions
- Triggers and event callbacks should recognize mercenaries as the active player.
- Test buttons and event hit registrations such as HitBubbleWand, HitSlam, HitBowl, and HitCruise where relevant.
- Integrate hazards, pickups, collectibles, checkpoints, portals/warps, and NPC/player detection through existing BFBB systems.
- Preserve BFBB progression and rewards instead of implementing TF2-side imitations.

### 5. Scripted sequences and presentation
- Handle CAM and FLY sequences, temporarily controlling player input and camera as BFBB requires.
- Suppress/hide TF2 HUD when appropriate during scripted scenes and restore it on control return.
- **BFBB load screen (decided 2026-10-29).** A **client-side** panel, not TF2's map-change screen: TF2's "You're on your way to" layout only appears during a real map change, which the bridge deliberately avoids. Shown on a `LOADING` state flag while BFBB loads a scene, **opaque and full-screen** so BFBB's bubble wipe and TF2's HUD are both hidden, then handed back on return. Content: **BFBB level title, one still image of that level, and a progress bar.** The bar is time-based until BFBB exposes a real load percentage -- prefer an indeterminate crawl to a bar that lies at 90%.
- Investigate first-person viewmodels, taunts/third-person presentation, mercenary animation/jaw integration, audio, lighting, and material/surface mapping.
- Keep TF2 VTF/VMT assets on the TF2 side; investigate mapping BFBB surface/material identities to TF2 surface properties rather than manually authoring a huge per-level list.

### 6. Broaden collision and world response
- Extend beyond JSP to PLAT, SIMP, NPC collision, and other dynamic/interactive objects.
- Do this only with deliberate test scenes and separate validation for each category.
- Later revisit impact normals, material IDs, surface properties, decals, particles, and sounds when effects integration is in scope.

### 7. Full-game validation
- Test all nine mercenaries.
- Play representative levels through progression, collectibles, combat, triggers, scripted events, deaths, checkpoints, warps, and transitions.
- Treat successful completion of BFBB as the ultimate acceptance test.

## Parked design decisions and future tasks

These come from `roadmapmessage.txt` at the BFBB repo root and from the original scoping conversation. None of them is scheduled; they are recorded so decisions made in passing are not lost. The roadmap's numbered sections above stay the plan; this is the detail behind them.

### Robot engagement: the MVPT arena problem

BFBB robots wake when SpongeBob walks into an MVPT arena radius, then either shoot (projectile types) or walk straight at him, and they forget him when he leaves. In source, `zNPCGoalAlert::Process()` returns to idle once `npc->arena.IsReady()` is false and steers toward `globals.player.ent`, not toward an arbitrary attacker. A merc firing hitscan from a distant platform is therefore outside the encounter, and the robot can go idle while it is being shot, or chase SpongeBob while the merc is the one fighting it.

Direction if taken up: keep the native arena as the boundary of where a robot may operate, add combat awareness fed by bridge hits, track a combat target separately from the native player target, and de-aggro on a timeout rather than instantly, so one stray bullet does not wake the level. Do not rewrite each robot's goal tree. The next source investigation is the arena itself (`IncludesPlayer()`, `IncludesPos()`, the code that builds MVPT and radius) and the projectile alert callbacks.

### Robot knockback ownership

Native `zNPCGoalKnock::Enter()` adds its own fixed launch impulse to `frame->oldvel`, and `CollReview()` owns what follows: `NPCC_Bounce()` reflection through `fac_elastic` after 0.1 s, damaging surfaces and goo on their own path, `DMGTYP_HITBYTOSS` when NPCs collide. So TF2 should supply the impulse and BFBB should keep collision resolution; injecting a force before damage can be overwritten by that `Enter()`. Undecided: whether to transmit a raw force vector, a direction plus magnitude, or weapon-specific force data — TF2's own helpers disagree (bullet force from the ammo definition, melee scaling with base damage, explosive clamped and randomised). Only the one-shot melee override (`zTF2Bridge_ConsumeKnockbackOverride`) exists so far.

### Robot damage model: BFBB few-hit kills vs TF2 health pools

- **Option A (current).** Native BFBB hits: a robot dies in the same one, two or three hits regardless of weapon. Cheap and faithful, but it breaks explosive juggling (Soldier, Demo, Scorch Shot) and makes hitscan and melee very strong, which is what pushed the arena question above.
- **Option B.** Give robots health pools balanced around TF2 damage. Tankier and fairer weapon-to-weapon, but sniper headshots then need per-robot head positions, and several robots have awkward heads.
- Wanted in either case: a convar or options-menu toggle for the mode, and probably a damage/difficulty multiplier.

Nothing here is started. The bridge spends the player's health only; NPC damage stays native.

### Bungees

A whole separate moveset with its own animations and a forced third person, required for some spatulas and socks. Not a priority during core functionality, but it blocks full-game playability and needs a real design rather than an adapter.

### BFCL generation and storage

Today one generated BFCL is written per level. Revisit whether levels should keep their collision files long term — less regeneration, and the same geometry could seed nav-meshes if TF2 bots are ever wanted. No decision yet.

### Fire and stage hazards

- Pyro as a class needs fire damage over time delivered into BFBB and rendered on the robot the way TF2 does it.
- Slick's oil and the ice floors need ice physics; bounce pads need forwarding.
- Rocket jumping used to be eaten by TF2 collision before BFBB could observe the impact; that is fixed and juggling works now.

### Wall-piercing hitscan

Early builds let weapons damage robots through geometry. BFBB occlusion is in the hitscan path now and verified; keep it that way rather than restoring the debug behaviour.

### Multiplayer and editor integration (explicitly not core)

**Settled with the user on 2026-10-29, when we get there:**

- **Co-op in one scene first.** Two or more players in the same level, same robots, same NPCs. No per-player scenes, no cross-level streaming, and the "a robot respawned in your friend's face" problem does not exist because there is only one scene. The hard part is synchronisation, not rendering.
- **Everyone is a copy of SpongeBob.** Each player is a real BFBB player entity in the shared scene, not one host player plus companions. This needs no new per-entity plumbing beyond what per-player scenes would require anyway, and it keeps checkpoints, warps and cutscenes meaningful for everyone.
- **Consequence, not yet built: robots need a target priority.** Every robot goal steers at `globals.player.ent`. With two or more players in one MVPT radius, each robot has to pick one (nearest, most-damaging, or the one in front of it) and re-evaluate, or two players will fight a robot that is looking at neither. This is the first piece of real co-op work and it belongs with the MVPT awareness task below.
- **Two levels at once is a later exploration, not an assumption.** If per-player scenes turn out unviable, co-op alone is an acceptable end state. The user wants the options and the cost understood either way, not a promise.

- Multiplayer or co-op with independent scenes: one player in Jellyfish Fields while another is in Bikini Bottom, no global reset when one of them dies. Likely deep core work in BFBB.
- Industrial Park editor support for Hammer-style extras: sentry build spots, player and bot spawn points as pointer assets, MvM-like opposing spawns.

## Relevant BFBB asset categories (planning reference)

- World geometry/collision: JSP, BSP, BOUL, COLL, DSCO, DSTR, MINF, PLAT, SIMP, VIL/MODL (actual collision role to be verified per type).
- Interactions/progression: TRIG, EGEN, MRKR, MVPT, PICK, PKUP, PORT, PLYR.
- Presentation/sequences: CAM, FLY, JAW, UI, UIFT, ANIM, RWTX, LITE, FOG, SND/SNDI/SNDS.
- These are investigation leads based on project context, not a claim that every type has been audited.

## Known caveats / do not regress

- The rocket splash solution is a pragmatic compromise. The user chose the current wrap behavior; avoid spending more time perfecting it absent a reproducible blocker.
- The rocket debug lifetime setting should affect marker duration, not whether rockets function.
- TF2-side projectile visuals/tracers may not match BFBB geometry perfectly yet because TF2 does not natively know the BFBB collision world.
- Rocket debug markers and splash exposure diagnostics are development tools, not final visuals.
- Do not add PLAT/SIMP collision and then claim it works without a deliberately prepared test.
- Do not remove actual BFBB NPC damage merely because damage/health design remains undecided; keep behavior configurable if useful for diagnostics and future testing.
- A robot hit must always shove, even point blank. The direction comes from the robot's origin, but the hammer's swing and a bomb bot's burst happen with the robot's origin inside the player's hull, where that offset is smaller than eight units and carries no usable direction. `TF2Bridge_ApplyPlayerHit` falls back to the player's horizontal velocity (reversed), then to straight back from the view, instead of silently skipping the impulse. Never restore the old "no direction means no shove" behaviour; the debug line's `dist=` and `dir=` show which path was taken.
- The player shove is an impulse plus a deliberate ground pop. With the impulse alone, a logged `push=618` at `dist=128` (bomb bot) was not felt in game: on the ground, friction spends it over a few ticks and it reads as a slide. `TF2Bridge_ApplyPlayerHit` clears `FL_ONGROUND` and the ground entity before `ApplyAbsVelocityImpulse`, the way airblast does. Do not "clean up" that flag clearing as redundant; without it the shove is invisible at these magnitudes. Unverified in game at the time of writing.
- `DMG_CLUB | DMG_PREVENT_PHYSICS_FORCE` on the bridge's player hit is deliberate. `DMG_CLUB` is what earns the pain sound, the flinch and the directed HUD arrow; the second bit is how the engine is told the hit carries no physics force on purpose, so `CBaseEntity::TakeDamage` stops printing `inputInfo.GetDamageForce() == vec3_origin`. Do not "fix" that console line by dropping the flag or by handing the info a real explosive force.

## Update protocol

When a meaningful change is made, update this file with:
1. What changed and which branch/commit contains it.
2. Whether it compiled (which side) and whether it was tested in game.
3. Exact test conditions and results.
4. Known regressions or unresolved questions.
5. The next single recommended task.

Never mark a feature “verified” based solely on code inspection or a successful compile.
