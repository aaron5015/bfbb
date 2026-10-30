# Snapshot tests

Each case is a tour script (`<case>.tour`, grammar in
[iTour.h](../../src/SB/Core/pc/iTour.h)) and the `config.ini` it runs with
(`<case>.ini`). Each `shot NAME` in a script is compared against `NAME.png`
in this folder. The main README's banner is `title_1080p.png`.

```sh
python tools/snapshots.py                  # run every case
python tools/snapshots.py menus_1080p      # run one
python tools/snapshots.py 'reg_*'          # run the cases a glob matches
python tools/snapshots.py --update         # rewrite shots that are new or differ
```

The `reg_` cases are the regression suite: the game as the Xbox drew it,
640x480 on Direct3D 9. One case per scene boots straight into it with
`[game] boot` and shoots SpongeBob at the spawn point, or the cutscene and the
fight for a boss. The rest cover the menus, the pause menu, the `[cheats]`
that change what is on screen, and the render settings, on the Jellyfish Rock
spawn. The other cases are the README's screenshots.

The script runs `bin/bfbb.exe` (`--exe` for another build) hidden, with a
scratch config, an empty save folder and no audio. The assets come from
`BFBB_ASSETS`, or from `[assets] path` in the `config.ini` beside the
executable. Needs Pillow and NumPy.

In a tour the game clock advances exactly 1/60 s per frame and the random seed
is pinned (`seed` in iTour.h), so a run is pixel-identical to the last one on
the same machine. The seed is set at each level start and again when play
begins, so what came before a level does not move its randomness. What a
level draws while it initialises still depends on every object loaded with
it, the always-loaded menus included: a change to the menu data can move the
boss cases. Sound is silent and plays out on the same clock, and nothing caps the
frame rate: the whole `reg_` suite runs in about a minute and a half. A shot passes at a mean difference of 2.0 or less per channel, out of
255 (`--tolerance`). Another GPU or driver can land above that; check the diff
image the script leaves in its scratch folder before updating.

Frame counts in the scripts are from that fixed clock. If a scene's timing
changes, probe it with a `shot` every 60 frames and move the waits.
