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
| `sndcheck.py`, `sndcheck.cpp` | decodes every GameCube sound with `iSndDsp.cpp` and compares it with its Xbox twin |
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

## Sound

```sh
python tools/gcswap/sndcheck.py <gamecube files/> <xbox assets/>
```

Pairs each GameCube package that has an SNDI with the Xbox package at the same
path, swaps the table, decodes every sound it names, and compares rate, length,
loop flag and the correlation of the two waveforms. A sound authored at a
different rate in the two builds is compared at the lower rate.

Against the retail discs: 63 tables, 1,751 sounds (485 sfx, 1,236 streams, 30
cutscene). 977 share the Xbox rate; their median correlation is 0.99994, and
975 of them are above 0.99. 763 are at half or double the Xbox rate. Of all
1,751, 1,749 correlate above 0.95. The other two (58a8dace, 4bd3223e) are
different recordings. Loop flags agree on 1,748 of 1,751. No loop starts past
sample 0. All 30 cutscene sounds carry a copy of their header.
