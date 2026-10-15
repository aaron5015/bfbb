# TF2-in-BFBB Bridge — Roadmap and Current Status

Last updated: 2026-10-15

This document is a project-status supplement, not a replacement for repository history or the existing BFBB buddy/NME project notes. Update it when a milestone is verified or the current task changes.

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
- Rocket splash damage/exposure has been implemented and iterated. The user chose to keep the current generous wrap-around compromise rather than spend more time revisiting the edge cases. Do not reopen this unless a repeatable gameplay issue warrants it.
- BFBB native NPC damage/death/reward behavior remains the current default model. Damage has been applied during prototype testing; the long-term choice between BFBB-style damage and an optional TF2-style health/damage model is undecided.
- Current testing has focused on NPCs and JSP/static world collision. Do not assume PLAT/SIMP collision has been tested.

## Current task / next milestone

**Next contained weapon milestone: rocket knockback / explosive behavior.**

Rocket impact, explosion visualization, and NPC splash damage are far enough along to move forward. Implement knockback away from the explosion and, where practical, TF2-like vertical juggling. Validate direction and strength with direct hits, nearby splash, and targets at different relative heights. Keep the current splash-wrap behavior unchanged during this task unless a clear regression blocks testing.

After that, prioritize player integration and a reliable playable loop rather than completing every weapon first.

## Roadmap

### 1. Player-control contract and camera synchronization
- Define player authority for normal play, cutscenes, flythroughs, death, respawn, warps, and level changes.
- Keep TF2 view direction and BFBB camera orientation aligned.
- Ensure BFBB player facing/movement is consistent with the mercenary, rather than treating the BFBB player as a position-only teleported proxy.
- Prevent snapping, stale facing, and incorrect movement axes during handoffs.
- Make player-dependent native BFBB systems recognize the active mercenary consistently; revisit cases such as Sleepy detection that may depend on normal player movement/update flow.

### 2. Reliable end-to-end playable loop
- Spawn, move, jump, take damage, die, and respawn.
- Activate a checkpoint and return to its location after death.
- Collect a pickup and verify BFBB's native progression/reward behavior.
- Trigger a level transition/warp and ensure TF2 follows correctly.
- Use this as a recurring integration test rather than testing only isolated weapons.

### 3. Weapon foundation
- Rocket knockback and explosive juggling (next weapon task).
- Grenade Launcher: collision, bouncing, fuse, detonation, and lifecycle.
- Stickybombs: surface attachment, arming, detonation, cleanup.
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

## Update protocol

When a meaningful change is made, update this file with:
1. What changed and which branch/commit contains it.
2. Whether it compiled (which side) and whether it was tested in game.
3. Exact test conditions and results.
4. Known regressions or unresolved questions.
5. The next single recommended task.

Never mark a feature “verified” based solely on code inspection or a successful compile.
