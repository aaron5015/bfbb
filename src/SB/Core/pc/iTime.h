#ifndef ITIME_H
#define ITIME_H

#include <types.h>

#define JANUARY 1
#define FEBRUARY 2
#define MARCH 3
#define APRIL 4
#define MAY 5
#define JUNE 6
#define JULY 7
#define AUGUST 8
#define SEPTEMBER 9
#define OCTOBER 10
#define NOVEMBER 11
#define DECEMBER 12

typedef S64 iTime;

// The GameCube's iTime is a raw timebase tick: the bus clock divided by four,
// which is why game code converts with GET_BUS_FREQUENCY() / 4 rather than
// calling iTimeDiffSec. Five files in src/SB do that arithmetic inline, so the
// host tick rate is not free -- GET_BUS_FREQUENCY() in iSystem.h is defined
// from this constant so that those five sites stay correct without edits.
//
// A microsecond tick keeps F32 precision on par with retail's 40.5 MHz one
// while staying exactly representable.
#define ITIME_TICKS_PER_SECOND 1000000

S32 iGetMinute();
S32 iGetHour();
S32 iGetDay();
S32 iGetMonth();
U32 iGetCurrFormattedDate(char* input);
U32 iGetCurrFormattedTime(char* input);
void iTimeInit();
void iTimeExit();
iTime iTimeGet();
F32 iTimeDiffSec(iTime t0, iTime t1);
F32 iTimeDiffSec(iTime time);
// Stop and restart iTimeGet's clock. A phone blocks the game thread while the
// app is in the background, and without this the first frame back would see
// the whole absence as one frame's delta. The clock reads the moment of
// suspension until it is resumed.
void iTimeSuspend();
void iTimeResume();
// Replace the host clock with one that starts at zero and moves only when
// iTimeStep is called, by 1/fps of a second each time. For iTour.h, which
// steps it once per presented frame; iVSync steps it too. With every frame's
// delta and the random seed taken from this clock, a tour plays the same way
// each run.
void iTimeFixedStep(U32 fps);
void iTimeStep();
// The fixed-step clock in nanoseconds into *ns, and TRUE; FALSE when
// iTimeFixedStep is not in use. What iVSync and the sound mixer read in a tour
// instead of the host clock.
S32 iTimeFixedNs(U64* ns);
// What zGameInit and zMenuInit seed xrand with: the clock, as retail does,
// unless iTimeSetSeed pinned a value. A tour pins one (iTour.h), because the
// fixed clock at a level's start counts every frame presented before it, and
// those change with anything that loads at boot. gc/iTime.h makes it
// iTimeGet().
U32 iTimeSeed();
void iTimeSetSeed(U32 seed);
// Seed xrand again as zGameLoop starts, when iTimeSetSeed pinned one; nothing
// otherwise, as retail. How many draws a level's init makes depends on every
// object in the scene, the always-loaded menu packages among them: four fewer
// mnu4 widgets changed the state play began with. The draws made during init
// still follow such changes. gc/iTime.h makes it nothing.
//
// Inline so that only the game code calling it needs xsrand: the platform
// layer links into pc_selftest, which has no xMath.cpp.
S32 iTimeSeedPinned();
void xsrand(U32 seed);
inline void iTimeSeedPlay()
{
    if (iTimeSeedPinned())
    {
        xsrand(iTimeSeed());
    }
}
void iTimeGameAdvance(F32 elapsed);
void iTimeSetGame(F32 time);
void iProfileClear(U32 sceneID);
void iFuncProfileDump();
void iFuncProfileParse(char* elfPath, S32 profile);

#endif
