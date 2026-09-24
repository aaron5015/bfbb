#ifndef IPAD_H
#define IPAD_H

#include <types.h>

struct _tagiPad
{
    S32 port;
};

struct _tagxPad;
struct _tagxRumble;

S32 iPadInit();
_tagxPad* iPadEnable(_tagxPad* pad, S16 port);
S32 iPadConvStick(float value);
S32 iPadUpdate(_tagxPad* pad, U32* on);
S32 iPadConvFromGCN(U32 a, U32 b, U32 c);
void iPadRumbleFx(_tagxPad* p, _tagxRumble* r, F32 time_passed);
void iPadStopRumble(_tagxPad* pad);
void iPadStopRumble();
void iPadStartRumble(_tagxPad* pad, _tagxRumble* rumble);
void iPadKill();

// The pad bits a talk box's links were authored against. The GameCube's
// archives agree with its pad, so this is the identity -- a macro, because an
// inline function still changes CodeWarrior's code for the caller.
#define iPadTalkBoxButtons(pressed) (pressed)

#endif
