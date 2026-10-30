#include "iModPatch.h"

// GameCube .sbss between gNumTemporaryLights and sSparkEmitter: four bytes of
// alignment padding.
static const U32 kPadAddress = 0x803CB8F4;
static U8 sPad[4];

U8* iModPatchByte(U32 address)
{
    if (address - kPadAddress < sizeof(sPad))
    {
        return &sPad[address - kPadAddress];
    }
    return NULL;
}

bool iModPatchSpinHover()
{
    return sPad[0] == 1;
}

bool iModPatchBowlFullTurn()
{
    return sPad[1] != 0;
}
