# gcswap

Byte-swap rules for GameCube assets, so the PC port can read a GameCube
package the way it reads an Xbox one. The runtime is
`src/SB/Core/pc/iGCSwap.cpp`; most of it is generated here.

| file | what it does |
| --- | --- |
| `dwarf.py` | struct layouts out of `dwarf/dwarf_data.cpp` (the PS2 build's DWARF) |
| `spec.py` | which structs each asset type is made of; the hand-decided cases |
| `gen.py` | writes `src/SB/Core/pc/iGCSwapTable.inc` from the two |
| `corpus.py` | pairs every GameCube asset with the Xbox asset of the same id |
| `check.py`, `check.cpp` | runs `iGCSwap.cpp` over the corpus and compares |
| `hipfile.py` | HIP/HOP reader, copied from `mods/`'s hipkit |

## Checking a change

```sh
python tools/gcswap/corpus.py <gamecube files/> <xbox assets/> corpus.bin
python tools/gcswap/check.py corpus.bin            # every type
python tools/gcswap/check.py corpus.bin DYNA        # one type, more detail
python tools/gcswap/check.py corpus.bin CAM 00999675 # one asset, word by word
```

`check.py` regenerates the table, builds the checker with clang++, and prints
per type how many pairs match the Xbox copy exactly after the swap. For the rest
it asks, word by word, whether some byte order of the raw GameCube word gives
the Xbox one. If one does, a rule is wrong there and the report names the
offset. If none does, the two builds shipped different values: content, not
layout.

## State

Against the retail GameCube and Xbox discs: 33,896 pairs, 32,769 the same size.
All but 11 of those either match exactly or differ only in content. The 11 are
CRDT (2), SHRP (5), SURF (1), TEXT (2) and PARS (1).

ANIM: 840 of 1,031 match exactly; the other 191 differ only in keyframe values
off by one, which the two builds quantized differently.

JAW: 48 of 49 match exactly; the other is a different table.

SNDI keeps the GameCube layout (`iGCSwapSoundTable`); `iSnd.cpp` reads both.
RenderWare streams (MODL, RWTX, JSP) and the samples in SND and SNDS are not
byte-swapped at all.
