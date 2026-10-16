// Button-prompt glyphs read from loose PNGs.
//
// The interface and the argument for it are in iPadGlyph.h.
//
// It reaches past the RenderWare shim into librw for one thing -- decoding a
// PNG, which the C API has no call for and librw already carries lodepng for --
// and does the rest through the shim like any other platform file. It cannot
// live beside the shim in rw/: it asks the input backend what is bound to what,
// and bfbb_rw does not link bfbb_platform.

#include <rwcore.h>

#include "rw.h"

#include "iHost.h"
#include "iPadBind.h"
#include "iPadGlyph.h"
#include "iPadHost.h"

#include "iPadLayout.h"
#include "xPad.h"

#include <stdio.h>
#include <string.h>

namespace
{
    // A set is a FOLDER OF FILES, not a fixed twelve.
    //
    // Retail has twelve prompt textures, so a console set has twelve pictures
    // and nothing forces a cache. A keyboard has no such number: a prompt draws
    // whatever key is bound to that button, and [keyboard] can name any of a
    // hundred. So a glyph is looked up by FILE NAME and cached as it is asked
    // for, and a set is however many files it happens to hold.
    //
    // The face four are named for their POSITION rather than for a letter,
    // because the letter is the one thing that is not shared: the button east
    // of jump is printed B on an Xbox pad and X on a GameCube one. A file
    // called b.png could mean either, and gamecube/b.png holding an X reads
    // like a bug rather than like the truth. tools/padglyphs.py writes the same
    // names.
    //
    // The two sticks are not inputs anything binds. They are the pictures
    // button_move_text and button_camtoggle_text ask for, and those prompts
    // mean "push the stick", not "press the click", so they resolve straight to
    // a file and never go near a binding.

    // A PAD binding is written in SDL's input names, and those four ARE
    // positions -- SDL calls the east button `b` whatever is printed on it --
    // so this is a rename, not a mapping.
    //
    // It must NOT be applied to a keyboard binding. There "a" is the A key, and
    // renaming it to "south" would draw a face button for it.
    struct TokenGlyph
    {
        const char* token;
        const char* glyph;
    };

    const TokenGlyph kTokenGlyphs[] = {
        { "a", "south" },
        { "b", "east" },
        { "x", "west" },
        { "y", "north" },
    };

    const S32 kTokenGlyphCount = (S32)(sizeof(kTokenGlyphs) / sizeof(kTokenGlyphs[0]));

    // What each of retail's twelve textures means, in the port's terms.
    //
    // `mask` is the game button the prompt is about, and the thing a binding is
    // looked up by. `fallback` is the glyph to draw when the binding cannot
    // name one input -- a chord, or nothing bound -- and is also what the file
    // names in a set are keyed to, since a set is extracted into the port's
    // slot order.
    //
    // pad_button_R2 carries XPAD_BUTTON_HUD rather than R2. button_status_text
    // is the only prompt that uses it, it is the HUD prompt, and every disc
    // drew its own HUD button in that slot: the GameCube's Z, the PS2's R2, the
    // Xbox's black button. The R2 bit has no prompt of its own.
    //
    // Nothing draws the close camera. No disc has a prompt for it, so giving it
    // a button of its own costs no picture.
    //
    // The face four are read off the discs, and all three agree. Each disc's
    // menu prompts name a picture, that picture names one of these textures,
    // and the texture is a picture of a physical button: cancel draws the
    // button printed B everywhere, options the one printed X, accept A and the
    // third choice Y. The code cancels on TRIANGLE and opens options on O, so
    // B is TRIANGLE and X is O -- which is gc/iPad.cpp's mapping as well.
    //
    // A MOVE is not read here. The consoles do disagree about those, and what
    // they disagree about is which bit the move reads rather than which button
    // the bit is; iPadLayout.h holds that, and iTextPatch.cpp points a move's
    // prompt at the picture to match.
    //
    // `fallback` is the Xbox set's own slot order, since that is the art this
    // resolves against: 1 is A, 2 is Y, 3 is B, 4 is X.
    struct Slot
    {
        const char* name;
        U32 mask;
        const char* fallback;
    };

    const Slot kSlots[] = {
        { "pad_button1", XPAD_BUTTON_X, "south" },
        { "pad_button2", XPAD_BUTTON_SQUARE, "north" },
        { "pad_button3", XPAD_BUTTON_TRIANGLE, "east" },
        { "pad_button4", XPAD_BUTTON_O, "west" },
        { "pad_button_L1", XPAD_BUTTON_L1, "lt" },
        { "pad_button_R1", XPAD_BUTTON_R1, "rt" },
        { "pad_button_L2", XPAD_BUTTON_L2, "lb" },
        { "pad_button_R2", XPAD_BUTTON_HUD, "rb" },
        { "pad_button_start", XPAD_BUTTON_START, "start" },
        { "pad_button_select", XPAD_BUTTON_SELECT, "back" },
        { "pad_button_L_analog", 0, "stick_left" },
        { "pad_button_R_analog", 0, "stick_right" },
    };

    const S32 kSlotCount = (S32)(sizeof(kSlots) / sizeof(kSlots[0]));

    S32 sEnabled;

    // What config.ini asked for, and what that resolved to. They differ when
    // the answer is "auto": `sActive` then holds whatever the pad turned out to
    // be, and a pad swapped mid-game changes it.
    char sChoice[64] = "auto";
    char sActive[64];
    char sRoot[512];

    // One file that has been asked for, whether or not it was there. `tex` is
    // NULL for a file a set does not hold, and the entry is kept anyway: a set
    // may carry only the glyphs it wants to override, and without the negative
    // answer every prompt of every frame would stat the same missing file.
    struct Glyph
    {
        char name[32];
        RwTexture* tex;
    };

    // Twelve is what retail's prompts can ask for at once. The slack is for a
    // keyboard set, where the twelve are drawn from a hundred possible keys and
    // a rebind mid-session asks for a file the cache has not seen.
    const S32 kGlyphMax = 48;

    Glyph sGlyph[kGlyphMax];
    S32 sGlyphCount;
    S32 sLoaded;

    // So an unreadable set says so once rather than once per frame per prompt.
    char sComplained[64];

    void dropGlyphs()
    {
        for (S32 i = 0; i < sGlyphCount; i++)
        {
            if (sGlyph[i].tex != NULL)
            {
                RwTextureDestroy(sGlyph[i].tex);
                sGlyph[i].tex = NULL;
            }
        }
        sGlyphCount = 0;
        sLoaded = 0;
    }

    // One PNG into one texture, or NULL. A set is allowed to be incomplete, so
    // a missing file is not a failure and not a diagnostic: it means "leave
    // that prompt as the disc drew it".
    RwTexture* loadGlyph(const char* set, const char* glyph)
    {
        char path[768];
        snprintf(path, sizeof(path), "%s/buttons/%s/%s.png", sRoot, set, glyph);

        // readPNG asserts on a file it cannot open, and an assert is compiled
        // out of a release build -- the nil then reaches lodepng and takes the
        // process with it. So the existence check is load-bearing, not a
        // courtesy.
        if (!iHostPathExists(path))
        {
            return NULL;
        }

        rw::Image* image = rw::readPNG(path);
        if (image == NULL)
        {
            printf("bfbb: %s is not a PNG this can read; that prompt stays as the disc drew it\n",
                   path);
            fflush(stdout);
            return NULL;
        }

        // The prompts are drawn over the game, so a set author's transparency
        // has to survive whatever bit depth they saved at.
        image->convertTo32();

        rw::Raster* raster = rw::Raster::createFromImage(image);
        image->destroy();

        if (raster == NULL)
        {
            printf("bfbb: no raster could be made for %s\n", path);
            fflush(stdout);
            return NULL;
        }

        rw::Texture* texture = rw::Texture::create(raster);
        if (texture == NULL)
        {
            raster->destroy();
            return NULL;
        }

        // Retail filters these linearly (xFont.cpp does it to every {tex:}
        // raster it resolves) and they are drawn at whatever size the text is,
        // so the same has to happen here or a set would look different from the
        // disc's own glyphs for no reason the player could name.
        texture->setFilter(rw::Texture::LINEAR);
        texture->setAddressU(rw::Texture::CLAMP);
        texture->setAddressV(rw::Texture::CLAMP);

        return reinterpret_cast<RwTexture*>(texture);
    }

    // Which set to draw. "auto" asks the backend and takes its answer; anything
    // else is a folder name and is used as written.
    const char* resolveSet()
    {
        if (iHostStrCaseCmp(sChoice, "auto") != 0)
        {
            return sChoice;
        }

        // A keyboard is a device with its own pictures, not the absence of a
        // pad, and with both plugged in it is the one last touched that the
        // prompts should show. Asked before iPadHostPadKind, which answers
        // about the controller whether or not anyone is holding it.
        if (iPadHostKeyboardIsLive())
        {
            return "keyboard";
        }

        const char* kind = iPadHostPadKind();
        if (kind != NULL)
        {
            return kind;
        }

        // No pad, or one the backend cannot place. Keep whatever is already
        // being drawn rather than snapping back: a wireless controller going to
        // sleep, or a cable pulled for a moment, would otherwise change every
        // prompt in the game and change them back. Only a pad the backend CAN
        // place moves the set.
        if (sLoaded && sActive[0] != '\0')
        {
            return sActive;
        }

        // Nothing has been drawn yet, so there is no last answer to keep. The
        // Xbox set is what the game's own files hold.
        return "xbox";
    }

    // Loads the set if it is not the one already loaded. False when there is
    // nothing to draw from.
    bool ensureSet()
    {
        const char* want = resolveSet();

        if (sLoaded && iHostStrCaseCmp(sActive, want) == 0)
        {
            return true;
        }

        // A set that is not there has already been reported, and this runs for
        // every prompt of every frame. Answering from the name alone keeps it
        // from being one stat() per prompt for the rest of the session.
        if (!sLoaded && iHostStrCaseCmp(sComplained, want) == 0)
        {
            return false;
        }

        dropGlyphs();

        char dir[700];
        snprintf(dir, sizeof(dir), "%s/buttons/%s", sRoot, want);
        if (!iHostPathExists(dir))
        {
            snprintf(sComplained, sizeof(sComplained), "%s", want);
            printf("bfbb: no button glyphs at %s -- prompts stay as the disc drew them\n", dir);
            fflush(stdout);
            sActive[0] = '\0';
            return false;
        }

        snprintf(sActive, sizeof(sActive), "%s", want);
        sLoaded = 1;

        // Printed on every change, not just the first, because under "auto"
        // this is what a controller being swapped mid-game looks like from
        // outside -- and a set that quietly did not change is the failure worth
        // seeing. No count to go with it: nothing has been read yet, and what a
        // set holds is not knowable without listing the folder.
        printf("bfbb: button prompts from buttons/%s\n", want);
        fflush(stdout);

        return true;
    }

    // The texture for one file of the live set, read the first time it is asked
    // for and remembered after -- including a miss, so a set that does not hold
    // it is not stat'd again every frame.
    RwTexture* glyphNamed(const char* file)
    {
        if (file == NULL || file[0] == '\0')
        {
            return NULL;
        }

        for (S32 i = 0; i < sGlyphCount; i++)
        {
            if (iHostStrCaseCmp(sGlyph[i].name, file) == 0)
            {
                return sGlyph[i].tex;
            }
        }

        if ((size_t)strlen(file) >= sizeof(sGlyph[0].name))
        {
            return NULL;
        }

        // Full. Only reachable by rebinding over and over in one session, and
        // starting the cache again costs one reload of what is on screen --
        // which is cheaper than growing without limit or never caching again.
        if (sGlyphCount == kGlyphMax)
        {
            const char* set = sActive;
            char keep[64];
            snprintf(keep, sizeof(keep), "%s", set);
            dropGlyphs();
            snprintf(sActive, sizeof(sActive), "%s", keep);
            sLoaded = 1;
        }

        Glyph* g = &sGlyph[sGlyphCount++];
        snprintf(g->name, sizeof(g->name), "%s", file);
        g->tex = loadGlyph(sActive, file);
        return g->tex;
    }

    // The FILE a bound input is drawn from. On a pad the four face names are
    // positions under another name and get renamed; on the keyboard they are
    // the letter keys and must be left exactly as they are.
    const char* glyphFileFor(const char* token, S32 fromKeyboard)
    {
        if (!fromKeyboard)
        {
            for (S32 i = 0; i < kTokenGlyphCount; i++)
            {
                if (iHostStrCaseCmp(kTokenGlyphs[i].token, token) == 0)
                {
                    return kTokenGlyphs[i].glyph;
                }
            }
        }

        return token;
    }
} // namespace

void iPadGlyphSetEnabled(S32 on)
{
    if (!on)
    {
        dropGlyphs();
    }
    sEnabled = on ? 1 : 0;
}

S32 iPadGlyphEnabled()
{
    return sEnabled;
}

void iPadGlyphSetChoice(const char* name)
{
    snprintf(sChoice, sizeof(sChoice), "%s", (name != NULL && name[0] != '\0') ? name : "auto");
    dropGlyphs();
    sComplained[0] = '\0';
}

void iPadGlyphSetRoot(const char* dir)
{
    snprintf(sRoot, sizeof(sRoot), "%s", (dir != NULL) ? dir : ".");
    dropGlyphs();
}

const char* iPadGlyphActiveSet()
{
    return (sLoaded && sActive[0] != '\0') ? sActive : NULL;
}

void iPadGlyphExit()
{
    dropGlyphs();
    sActive[0] = '\0';
}

RwTexture* iPadGlyphFor(const char* name, U32 nameLen)
{
    if (!sEnabled || name == NULL || nameLen == 0 || nameLen >= 64)
    {
        return NULL;
    }

    // xFont hands over a slice of the markup rather than a string, and this is
    // called for every {tex:} in the game -- most of which are not prompts --
    // so the copy happens before the table walk and nothing else does.
    char slot[64];
    memcpy(slot, name, nameLen);
    slot[nameLen] = '\0';

    const Slot* found = NULL;
    for (S32 i = 0; i < kSlotCount; i++)
    {
        if (iHostStrCaseCmp(kSlots[i].name, slot) == 0)
        {
            found = &kSlots[i];
            break;
        }
    }

    if (found == NULL || !ensureSet())
    {
        return NULL;
    }

    const char* file = found->fallback;

    // Where THIS set prints the button, when there turns out to be no binding
    // to ask. Without it a GameCube set would draw its X beside "Return to
    // Game", because the slot order the files are keyed to is the Xbox's.
    const char* printed = iPadLayoutFaceGlyph(sActive, found->mask);
    if (printed != NULL)
    {
        file = printed;
    }

    // A prompt about a game button asks what presses it. A prompt about a stick
    // (mask 0) has nothing to ask and goes straight to its picture.
    if (found->mask != 0)
    {
        const char* input = iPadHostBoundInput(found->mask);
        if (input != NULL)
        {
            const char* bound = glyphFileFor(input, iPadHostKeyboardIsLive());
            RwTexture* tex = glyphNamed(bound);
            if (tex != NULL)
            {
                return tex;
            }
        }
    }

    return glyphNamed(file);
}
