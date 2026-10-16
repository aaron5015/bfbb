// The in-game settings screen.
//
// A list of config.ini settings, a few rows at a time: up and down pick one,
// left and right change it, and the change is applied there and then --
// through the same setters iSystem.cpp's ApplyConfig uses at startup -- and
// written back to config.ini when the screen closes (iConfigSave). A setting
// that cannot change while the game runs says so and takes effect on the next
// start. A change to the window that could leave the player unable to see the
// game asks to be kept, and puts itself back if nobody answers.
//
// It is reached through the game's own save and load modes, so no game code
// knows it exists: the title's Settings entry switches to Load mode and the
// pause menu's to Save mode, exactly as their Load and Save entries do, and
// iSGLoadLoop / iSGSaveLoop (iSaveScreen.cpp) run this instead of a save
// screen when the entry that got them there is the Settings one.

#include "iSettingsScreen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rwcore.h>

#include "iAssetOverride.h"
#include "iBoot.h"
#include "iConfig.h"
#include "iPadBind.h"
#include "iPadHost.h"
#include "iDistort.h"
#include "iDrawDist.h"
#include "iGlow.h"
#include "iHost.h"
#include "iPadGlyph.h"
#include "iPadStick.h"
#include "iScreen.h"
#include "iMenuMouse.h"
#include "iMenuWide.h"
#include "iSnapshot.h"
#include "iTime.h"
#include "iWindow.h"
#include "iCamera.h"
#include "xEvent.h"
#include "xPad.h"
#include "xString.h"
#include "zGame.h"
#include "zGameState.h"
#include "zGlobals.h"
#include "zSaveLoad.h"
#include "zScene.h"
#include "zCamera.h"
#include "zUI.h"

namespace
{
    enum When
    {
        NOW,
        NEXT_AREA,
        RESTART
    };

    // The tabs, in order. The shoulder buttons move between them.
    enum Tab
    {
        TAB_DISPLAY,
        TAB_EFFECTS,
        TAB_CONTROLS,
        TAB_GAME,
        TAB_COUNT
    };
    const char* const kTabNames[TAB_COUNT] = { "Display", "Effects", "Controls", "Game" };

    struct Setting
    {
        Tab tab;
        const char* label;

        // The config.ini key. "video.resolution" is not one: it stands for
        // video.width and video.height together.
        const char* key;

        // The words the setting takes, '|' between them, and what the screen
        // calls each one. NULL names means the words themselves.
        const char* words;
        const char* names;

        When when;
        const char* help;

        // Applies a word. NULL for one that only takes effect on restart, or
        // that the game reads again itself.
        void (*apply)(const char* word);

        // Could leave the player without a usable picture, so it asks to be
        // kept.
        bool confirm;
    };

    bool On(const char* w)
    {
        return iHostStrCaseCmp(w, "on") == 0;
    }

    iWindowMode ModeFor(const char* w)
    {
        if (iHostStrCaseCmp(w, "windowed") == 0)
        {
            return iWINDOW_WINDOWED;
        }
        return iHostStrCaseCmp(w, "fullscreen") == 0 ? iWINDOW_FULLSCREEN : iWINDOW_BORDERLESS;
    }

    void ApplyMode(const char* w)
    {
        iWindowSetMode(ModeFor(w));
    }

    void ApplyVSync(const char* w)
    {
        iWindowSetVSync(On(w));
    }

    void ApplyFrameRate(const char* w)
    {
        S32 fps;
        if (iHostStrCaseCmp(w, "display") == 0)
        {
            fps = iWindowGetDisplayRefreshRate();
        }
        else if (iHostStrCaseCmp(w, "off") == 0)
        {
            fps = 0;
        }
        else
        {
            fps = atoi(w);
        }
        iWindowSetFrameRate(fps > 0 ? fps : 0);
    }

    void ApplyFOV(const char* w)
    {
        iScreenSetFOV((F32)atof(w));
    }

    void ApplyUI(const char* w)
    {
        iScreenSetUIMode(iHostStrCaseCmp(w, "native") == 0 ? iSCREENUI_NATIVE : iSCREENUI_PILLARBOX);
    }

    void ApplyDrawDistance(const char* w)
    {
        iDrawDistSetUnlimited(On(w));
        iCameraSetNearFarClip(0.0f, iDrawDistFarClip());
    }

    void ApplyGlow(const char* w)
    {
        iGlowSetEnabled(On(w));
    }

    void ApplyDistortion(const char* w)
    {
        iDistortSetEnabled(On(w));
    }

    void ApplySnapshot(const char* w)
    {
        iSnapshotSetEnabled(On(w));
    }

    void ApplyDeadzone(const char* w)
    {
        iPadStickSetDeadzone(iHostStrCaseCmp(w, "auto") == 0 ? -1.0f : (F32)atof(w));
    }

    void ApplySticks(const char* w)
    {
        iPadStickSetSwap(iHostStrCaseCmp(w, "swapped") == 0);
    }

    // Both axes, from config.ini: Store has already written this one there.
    void ApplyInvert(const char*)
    {
        iPadStickSetInvert(iConfigGetBool("input.invert_camera_x", FALSE),
                           iConfigGetBool("input.invert_camera_y", FALSE));
    }

    // The startup applied the speed by multiplying the base scales (zMain.cpp,
    // after SB.INI); a change multiplies them again by new over old, so
    // SB.INI's base survives however often it is changed.
    void ApplyCameraSpeed(const char* w)
    {
        const F32 was = iBootCameraSensitivity();
        const F32 now = (F32)atof(w);
        if (was <= 0.0f || now <= 0.0f)
        {
            return;
        }
        zcam_pad_pyaw_scale *= now / was;
        zcam_pad_pitch_scale *= now / was;
        iBootSetCameraSensitivity(now);
    }

    void ApplyIcons(const char* w)
    {
        iPadGlyphSetChoice(w);
        iPadGlyphSetEnabled(iHostStrCaseCmp(w, "off") != 0);
    }

    const Setting kSettings[] = {
        { TAB_DISPLAY, "Window", "video.mode", "borderless|windowed|fullscreen",
          "Borderless|Windowed|Fullscreen", NOW,
          "Borderless fills the screen as a window. Alt+Enter also switches.",
          ApplyMode, true },
        { TAB_DISPLAY, "Resolution", "video.resolution", NULL, NULL, RESTART,
          "Rendering resolution. The image is scaled to fit the window.", NULL, false },
        { TAB_DISPLAY, "VSync", "video.vsync", "on|off", "On|Off", NOW,
          "Wait for the display's refresh before showing a frame. Prevents tearing.", ApplyVSync,
          false },
        { TAB_DISPLAY, "Frame rate limit", "video.framerate", "30|60|120|144|165|240|display|off",
          "30|60|120|144|165|240|Monitor|Unlimited", NOW,
          "Maximum frames per second. 60 is the original.", ApplyFrameRate,
          false },
        { TAB_DISPLAY, "Field of view", "video.fov", "60|65|70|75|80|85|90|95|100|105|110", NULL, NOW,
          "Horizontal field of view in degrees at 4:3. 75 is the original.",
          ApplyFOV, false },
        { TAB_DISPLAY, "HUD layout", "video.ui", "pillarbox|native", "4:3 (original)|Screen edges", NOW,
          "HUD position on wide screens.", ApplyUI, false },
        { TAB_DISPLAY, "Draw distance", "video.draw_distance", "on|off", "Unlimited|Console", NEXT_AREA,
          "How far away objects are drawn.", ApplyDrawDistance, false },
        { TAB_DISPLAY, "Anti-aliasing", "video.msaa", "1|2|4|8", "Off|2x|4x|8x", RESTART,
          "Smooths jagged edges. Lowers performance.", NULL, false },
        { TAB_EFFECTS, "Glow", "xbox.glow", "on|off", "On|Off", NOW,
          "Xbox bloom effect.", ApplyGlow, false },
        { TAB_EFFECTS, "Screen warps", "xbox.distortion", "on|off", "On|Off", NOW,
          "Cruise Bubble screen distortion.", ApplyDistortion,
          false },
        { TAB_EFFECTS, "Loading-screen still", "xbox.snapshot", "on|off", "On|Off", NOW,
          "Show a still of the previous level on the loading screen. Also used for save pictures.",
          ApplySnapshot, false },
        { TAB_EFFECTS, "Cave echo", "xbox.reverb", "on|off", "On|Off", NEXT_AREA,
          "Reverb in caves and the Mermalair.", NULL, false },
        { TAB_CONTROLS, "Keyboard buttons", "bind.keyboard", "", "Change", NOW,
          "Keys for each game button.", NULL, false },
        { TAB_CONTROLS, "Controller buttons", "bind.pad", "", "Change", NOW,
          "Controller buttons for each game button.", NULL, false },
        { TAB_CONTROLS, "Stick deadzone", "input.deadzone", "auto|5|10|15|20|25|30",
          "Auto|5%|10%|15%|20%|25%|30%", NOW,
          "How far a stick moves before input registers.", ApplyDeadzone, false },
        { TAB_CONTROLS, "Button pictures", "input.button_icons", "auto|xbox|gamecube|ps2|off",
          "Auto|Xbox|GameCube|PlayStation|Original", NOW,
          "Controller type shown in button prompts.", ApplyIcons, false },
        { TAB_CONTROLS, "Camera speed", "input.camera_sensitivity", "0.5|0.75|1.0|1.25|1.5|2.0",
          "0.5x|0.75x|1x|1.25x|1.5x|2x", NOW, "Camera stick speed.",
          ApplyCameraSpeed, false },
        { TAB_CONTROLS, "Sticks", "input.sticks", "normal|swapped", "Normal|Swapped", NOW,
          "Normal: left stick moves, right stick turns the camera.", ApplySticks, false },
        { TAB_CONTROLS, "Invert camera X", "input.invert_camera_x", "off|on", "Off|On", NOW,
          "Reverse the camera's left and right.", ApplyInvert, false },
        { TAB_CONTROLS, "Invert camera Y", "input.invert_camera_y", "off|on", "Off|On", NOW,
          "Reverse the camera's up and down.", ApplyInvert, false },
        { TAB_GAME, "Intro movies", "game.intro_movies", "on|off", "On|Off", RESTART,
          "Logos shown at startup.", NULL, false },
    };
    const S32 kCount = (S32)(sizeof(kSettings) / sizeof(kSettings[0]));

    // The resolutions offered, and the display's own when it is not one of
    // them. Built when the screen opens.
    char sResWords[256];

    // The tab showing, and the settings on it: sSel and sTop index this list.
    Tab sTab;
    S32 sList[kCount];
    S32 sListCount;

    S32 sSel;
    S32 sTop;
    bool sRestart;

    // The tab under the menu pointer, or -1.
    S32 sHoverTab = -1;

    void BuildList()
    {
        sListCount = 0;
        for (S32 i = 0; i < kCount; i++)
        {
            if (kSettings[i].tab == sTab)
            {
                sList[sListCount++] = i;
            }
        }
    }

    const Setting& Selected()
    {
        return kSettings[sList[sSel]];
    }

    void Send(const char* name, U32 event)
    {
        xBase* to = zSceneFindObject(xStrHash(name));
        if (to != NULL)
        {
            zEntEvent(to, event);
        }
    }

    // The n'th '|' word of `list`, or false past the end.
    bool Word(const char* list, S32 n, char* out, size_t size)
    {
        const char* p = list;
        for (S32 i = 0; p != NULL; i++)
        {
            const char* bar = strchr(p, '|');
            size_t len = bar != NULL ? (size_t)(bar - p) : strlen(p);
            if (i == n)
            {
                if (len >= size)
                {
                    len = size - 1;
                }
                memcpy(out, p, len);
                out[len] = '\0';
                return true;
            }
            p = bar != NULL ? bar + 1 : NULL;
        }
        return false;
    }

    S32 WordCount(const char* list)
    {
        S32 n = 1;
        for (const char* p = list; *p != '\0'; p++)
        {
            n += *p == '|';
        }
        return n;
    }

    const char* Words(const Setting& s)
    {
        return s.words != NULL ? s.words : sResWords;
    }

    bool Profiled(const char* key)
    {
        return strcmp(key, "video.ui") == 0 || strcmp(key, "video.draw_distance") == 0 ||
               strcmp(key, "video.resolution") == 0;
    }

    bool CustomProfile()
    {
        return iHostStrCaseCmp(iConfigGetString("video.profile", "custom"), "custom") == 0;
    }

    // What the setting is now, as one of its words where it can be.
    void Current(const Setting& s, char* out, size_t size)
    {
        if (strcmp(s.key, "video.mode") == 0)
        {
            switch (iWindowGetMode())
            {
            case iWINDOW_WINDOWED:
                snprintf(out, size, "windowed");
                return;
            case iWINDOW_FULLSCREEN:
                snprintf(out, size, "fullscreen");
                return;
            default:
                snprintf(out, size, "borderless");
                return;
            }
        }

        // video.profile's vanilla and modern decide these three themselves;
        // what is showing is what they decided.
        if (Profiled(s.key) && !CustomProfile())
        {
            if (strcmp(s.key, "video.ui") == 0)
            {
                snprintf(out, size, iScreenGetUIMode() == iSCREENUI_NATIVE ? "native" : "pillarbox");
            }
            else if (strcmp(s.key, "video.draw_distance") == 0)
            {
                snprintf(out, size, iDrawDistUnlimited() ? "on" : "off");
            }
            else
            {
                snprintf(out, size, "%dx%d", (int)iScreenWidth(), (int)iScreenHeight());
            }
            return;
        }

        if (strcmp(s.key, "video.resolution") == 0)
        {
            snprintf(out, size, "%dx%d", (int)iConfigGetInt("video.width", 640),
                     (int)iConfigGetInt("video.height", 480));
            return;
        }

        snprintf(out, size, "%s", iConfigGetString(s.key, ""));
    }

    // Which word the setting is on. Numbers compare as numbers, so a file
    // saying 75.0 is on 75. -1 for a value the list does not have.
    S32 IndexOf(const Setting& s)
    {
        char cur[64];
        Current(s, cur, sizeof(cur));

        char w[64];
        for (S32 i = 0; Word(Words(s), i, w, sizeof(w)); i++)
        {
            if (iHostStrCaseCmp(w, cur) == 0)
            {
                return i;
            }
            char* end = NULL;
            const double a = strtod(w, &end);
            if (end != w && *end == '\0')
            {
                char* end2 = NULL;
                const double b = strtod(cur, &end2);
                if (end2 != cur && *end2 == '\0' && a == b)
                {
                    return i;
                }
            }
        }
        return -1;
    }

    void Name(const Setting& s, S32 i, char* out, size_t size)
    {
        if (i < 0)
        {
            Current(s, out, size);
            return;
        }
        if (s.names == NULL || !Word(s.names, i, out, size))
        {
            Word(Words(s), i, out, size);
        }
    }

    void BuildResolutions()
    {
        static const char* const kCommon[] = { "640x480",   "1280x720",  "1600x900",
                                               "1920x1080", "2560x1440", "3840x2160" };
        sResWords[0] = '\0';

        S32 dw = 0;
        S32 dh = 0;
        char native[32] = "";
        if (iWindowGetDisplaySize(&dw, &dh) && dw > 0 && dh > 0)
        {
            snprintf(native, sizeof(native), "%dx%d", (int)dw, (int)dh);
        }

        for (size_t i = 0; i < sizeof(kCommon) / sizeof(kCommon[0]); i++)
        {
            if (sResWords[0] != '\0')
            {
                strcat(sResWords, "|");
            }
            strcat(sResWords, kCommon[i]);
        }
        if (native[0] != '\0' && strstr(sResWords, native) == NULL)
        {
            strcat(sResWords, "|");
            strcat(sResWords, native);
        }
    }

    // video.profile's vanilla and modern set the render size, the HUD and the
    // draw distance themselves, and would override a change to any of them.
    // Changing one therefore turns the profile to custom, with the other two
    // pinned at what the profile had them on, so nothing else moves.
    void UnProfile()
    {
        if (CustomProfile())
        {
            return;
        }

        char v[32];
        snprintf(v, sizeof(v), "%d", (int)iScreenWidth());
        iConfigSet("video.width", v);
        snprintf(v, sizeof(v), "%d", (int)iScreenHeight());
        iConfigSet("video.height", v);
        iConfigSet("video.ui", iScreenGetUIMode() == iSCREENUI_NATIVE ? "native" : "pillarbox");
        iConfigSet("video.draw_distance", iDrawDistUnlimited() ? "on" : "off");
        iConfigSet("video.profile", "custom");
    }

    void Store(const Setting& s, const char* word)
    {
        if (Profiled(s.key))
        {
            UnProfile();
        }

        if (strcmp(s.key, "video.resolution") == 0)
        {
            int w = 0;
            int h = 0;
            if (sscanf(word, "%dx%d", &w, &h) == 2)
            {
                char v[16];
                snprintf(v, sizeof(v), "%d", w);
                iConfigSet("video.width", v);
                snprintf(v, sizeof(v), "%d", h);
                iConfigSet("video.height", v);
            }
            return;
        }

        iConfigSet(s.key, word);
    }

    void SetText(const char* fmt, S32 row, const char* text)
    {
        char name[32];
        sprintf(name, fmt, (int)row);
        iAssetTextSet(xStrHash(name), text);
    }

    void DrawHelp(const char* override)
    {
        char help[256];
        if (override != NULL)
        {
            snprintf(help, sizeof(help), "%s", override);
        }
        else
        {
            const Setting& s = Selected();
            const char* when = s.when == RESTART     ? "{n}Applies after restart."
                               : s.when == NEXT_AREA ? "{n}Applies in the next area."
                                                     : "{n}";
            snprintf(help, sizeof(help), "%s%s%s", s.help, when,
                     sRestart ? "{n}Some changes apply after restart." : "");
        }
        iAssetTextSet(xStrHash(ISETTINGS_HELP_TEXT), help);
    }

    bool IsBindEntry(const Setting& s)
    {
        return strncmp(s.key, "bind.", 5) == 0;
    }

    // The authored layout, spread toward the frame on a wide screen: labels
    // out to the left, values out to the right, the title and help as wide as
    // both. See iMenuWide.h.
    void Layout()
    {
        const F32 s = 0.6f * iMenuWideMargin();
        char name[32];

        iMenuWidePlace(ISETTINGS_TITLE, 45.0f - s, 560.0f + 2.0f * s);
        for (S32 row = 0; row < ISETTINGS_ROWS; row++)
        {
            sprintf(name, ISETTINGS_LABEL, (int)row);
            iMenuWidePlace(name, 55.0f - s, 270.0f + s);
            sprintf(name, ISETTINGS_VALUE, (int)row);
            iMenuWidePlace(name, 330.0f + s, 270.0f);
        }
        iMenuWidePlace(ISETTINGS_HELP, 55.0f - s, 545.0f + 2.0f * s);
    }

    // The tab bar: the shoulder buttons' pictures either side, the tab showing
    // in the menu's own dark teal and the rest faded toward the background.
    //
    // Cut off after tab `last`'s name for the pointer to measure, -1 for just
    // the first picture; TAB_COUNT is the whole bar.
    void TitleText(char* title, size_t size, S32 last)
    {
        // A tab under the pointer is brighter, in iMenuMouseBrighten's teal.
        U8 r = 0;
        U8 g = 70;
        U8 b = 85;
        iMenuMouseBrighten(&r, &g, &b);
        char hover[16];
        snprintf(hover, sizeof(hover), "{c=ff%02x%02x%02x}", r, g, b);

        snprintf(title, size, "{i:button_picture_07}");
        for (S32 t = 0; t < TAB_COUNT && t <= last; t++)
        {
            const char* colour = (t == sTab) ? "" : (t == sHoverTab ? hover : "{c=ff6f98a3}");
            char one[64];
            snprintf(one, sizeof(one), "%s%s%s%s", t == 0 ? " " : "   ", colour, kTabNames[t],
                     t == sTab ? "" : "{~:c}");
            strncat(title, one, size - strlen(title) - 1);
        }
        if (last >= TAB_COUNT)
        {
            strncat(title, "   {i:button_picture_05}", size - strlen(title) - 1);
        }
    }

    // The tab under the pointer on the tab bar: a name, or the shoulder
    // picture either side, which turns the page the way its button does.
    // -1 when the pointer is not on the bar.
    S32 PointerTab()
    {
        xBase* bar = zSceneFindObject(xStrHash(ISETTINGS_TITLE));
        F32 px;
        F32 py;
        if (!iMenuMouseOver(bar) || !iMenuMousePosition(&px, &py))
        {
            return -1;
        }

        // Each tab takes the gap before its name as well as the name.
        char title[256];
        F32 end;
        TitleText(title, sizeof(title), -1);
        if (!iMenuMouseTextEnd(bar, title, &end) || px <= end)
        {
            return (sTab + TAB_COUNT - 1) % TAB_COUNT;
        }
        for (S32 t = 0; t < TAB_COUNT; t++)
        {
            TitleText(title, sizeof(title), t);
            if (iMenuMouseTextEnd(bar, title, &end) && px <= end + 4.0f)
            {
                return t;
            }
        }
        return (sTab + 1) % TAB_COUNT;
    }

    void Draw()
    {
        Layout();

        char title[256];
        TitleText(title, sizeof(title), TAB_COUNT);
        iAssetTextSet(xStrHash(ISETTINGS_TITLE_TEXT), title);

        for (S32 row = 0; row < ISETTINGS_ROWS; row++)
        {
            const S32 i = sTop + row;
            if (i >= sListCount)
            {
                SetText(ISETTINGS_LABEL_TEXT, row, "");
                SetText(ISETTINGS_VALUE_TEXT, row, "");
                continue;
            }

            const Setting& s = kSettings[sList[i]];
            char value[80];
            Name(s, IndexOf(s), value, sizeof(value));

            char shown[96];
            if (IsBindEntry(s))
            {
                snprintf(shown, sizeof(shown), "{i:button_picture_01} Change");
            }
            else
            {
                snprintf(shown, sizeof(shown), "< %s >", value);
            }
            SetText(ISETTINGS_LABEL_TEXT, row, s.label);
            SetText(ISETTINGS_VALUE_TEXT, row, shown);
        }

        DrawHelp(NULL);
    }

    void SelectRow(S32 row, bool on)
    {
        char name[32];
        sprintf(name, ISETTINGS_LABEL, (int)row);
        Send(name, on ? eEventUISelect : eEventUIUnselect);
        sprintf(name, ISETTINGS_VALUE, (int)row);
        Send(name, on ? eEventUISelect : eEventUIUnselect);
    }

    // The row on screen the pointer is over, label or value, or -1.
    S32 PointerRow()
    {
        for (S32 row = 0; row < ISETTINGS_ROWS && sTop + row < sListCount; row++)
        {
            char name[32];
            sprintf(name, ISETTINGS_LABEL, (int)row);
            if (iMenuMouseOver(zSceneFindObject(xStrHash(name))))
            {
                return row;
            }
            sprintf(name, ISETTINGS_VALUE, (int)row);
            if (iMenuMouseOver(zSceneFindObject(xStrHash(name))))
            {
                return row;
            }
        }
        return -1;
    }

    // "Keep this?" for a change that could leave the player without a picture
    // they can use. Anything but a yes within the time puts it back.
    bool Confirm()
    {
        const S32 kSeconds = 15;
        const iTime start = iTimeGet();

        for (;;)
        {
            const F32 elapsed = iTimeDiffSec(start, iTimeGet());
            const S32 left = kSeconds - (S32)elapsed;
            if (left <= 0)
            {
                return false;
            }

            char text[160];
            snprintf(text, sizeof(text),
                     "Keep this setting? {i:button_picture_01} Keep  {i:button_picture_03} Revert{n}"
                     "Reverting in %d.",
                     (int)left);
            DrawHelp(text);

            zSaveLoad_Tick();
            const U32 pressed = mPad[globals.currentActivePad].pressed;
            if (pressed & XPAD_BUTTON_X)
            {
                return true;
            }
            if (pressed & XPAD_BUTTON_TRIANGLE)
            {
                return false;
            }
        }
    }


    // ------------------------------------------------------------------
    // The binding pages: one row per button the game reads, what presses it,
    // and a row that puts them all back. Confirm on a row waits for the next
    // key or controller button and binds it; Escape, or eight seconds, leaves
    // it alone. Options binds nothing to the row, except on the rows the menus
    // need (MenuNeeds). Written to config.ini's [keyboard] or [pad] and read
    // again at once (iPadHostReloadBindings).

    // What each row of kPadBindButtons is called here. The game's own names
    // are the GameCube's, and what a button does depends on the preset, so the
    // help line carries the table's description instead.
    const char* const kBindLabels[] = {
        "A button", "B button", "X button", "Y button", "Z (camera close)", "Show HUD",
        "Cruise bubble", "Talk / bus", "Spare L (L2)", "Spare R (R2)", "Start (pause)", "Select",
        "D-pad up", "D-pad down", "D-pad left", "D-pad right",
    };
    const S32 kBindLabelCount = (S32)(sizeof(kBindLabels) / sizeof(kBindLabels[0]));

    // Rows: one per button, then the reset row.
    S32 BindRows()
    {
        return kPadBindButtonCount + 1;
    }

    void BindKey(bool pad, S32 row, char* out, size_t size)
    {
        snprintf(out, size, "%s.%s", pad ? "pad" : "keyboard", kPadBindButtons[row].name);
    }

    // Buttons this screen and the pause menu cannot be worked without. Clearing
    // one would leave no way back in here to undo it.
    bool MenuNeeds(S32 row)
    {
        const U32 mask = kPadBindButtons[row].mask;
        return mask == XPAD_BUTTON_X || mask == XPAD_BUTTON_TRIANGLE || mask == XPAD_BUTTON_UP ||
               mask == XPAD_BUTTON_DOWN || mask == XPAD_BUTTON_START;
    }

    // A help line naming the rows that one press of `row`'s input also sets,
    // or "" when there are none.
    void SharedWith(bool pad, S32 row, char* out, size_t size)
    {
        out[0] = '\0';

        const iPadBind* binds = iPadHostBindings(pad);
        if (binds == NULL)
        {
            return;
        }

        size_t have = 0;
        for (S32 i = 0; i < kPadBindButtonCount && i < IPAD_BIND_MAX_BUTTONS; i++)
        {
            if (i == row || !iPadBindOverlap(binds[row], binds[i]))
            {
                continue;
            }

            const char* label = i < kBindLabelCount ? kBindLabels[i] : kPadBindButtons[i].name;
            const int n = snprintf(out + have, size - have, "%s%s",
                                   have == 0 ? "{n}Shares an input with " : ", ", label);
            if (n < 0 || (size_t)n >= size - have)
            {
                out[have] = '\0';
                break;
            }
            have += (size_t)n;
        }
    }

    void DrawBindPage(bool pad, S32 sel, S32 top, const char* help)
    {
        Layout();
        iAssetTextSet(xStrHash(ISETTINGS_TITLE_TEXT),
                      pad ? "Controller buttons" : "Keyboard buttons");

        for (S32 r = 0; r < ISETTINGS_ROWS; r++)
        {
            const S32 i = top + r;
            if (i >= BindRows())
            {
                SetText(ISETTINGS_LABEL_TEXT, r, "");
                SetText(ISETTINGS_VALUE_TEXT, r, "");
                continue;
            }
            if (i == kPadBindButtonCount)
            {
                SetText(ISETTINGS_LABEL_TEXT, r, "Reset all");
                SetText(ISETTINGS_VALUE_TEXT, r, "{i:button_picture_01} Defaults");
                continue;
            }

            char key[64];
            BindKey(pad, i, key, sizeof(key));
            SetText(ISETTINGS_LABEL_TEXT, r,
                    i < kBindLabelCount ? kBindLabels[i] : kPadBindButtons[i].name);
            const char* bound = iConfigGetString(key, "");
            const bool nothing = bound[0] == '\0' || iHostStrCaseCmp(bound, "none") == 0;
            SetText(ISETTINGS_VALUE_TEXT, r, nothing ? "(nothing)" : bound);
        }

        if (help != NULL)
        {
            DrawHelp(help);
            return;
        }

        char text[256];
        if (sel == kPadBindButtonCount)
        {
            snprintf(text, sizeof(text),
                     "Reset every binding on this page to its default.{n}"
                     "{i:button_picture_03} Back");
        }
        else
        {
            const char* does = kPadBindButtons[sel].does;
            // Short enough that the line of buttons after it always fits in
            // the help text's 256 bytes (mnu4_text.cpp); SharedWith stops at a
            // whole name.
            char shared[100];
            SharedWith(pad, sel, shared, sizeof(shared));
            snprintf(text, sizeof(text),
                     "%s%s%s{n}{i:button_picture_01} Change  %s"
                     "{i:button_picture_03} Back",
                     does != NULL ? "Action: " : "", does != NULL ? does : "", shared,
                     MenuNeeds(sel) ? "" : "{i:button_picture_04} Clear  ");
        }
        DrawHelp(text);
    }

    // Wait for the next input on the device and bind it. FALSE if left alone.
    bool Capture(bool pad, S32 row)
    {
        const iTime start = iTimeGet();
        char token[32];

        // The confirm press that got here is still down: forget it first.
        iPadHostCaptureKey(TRUE, token, sizeof(token));
        iPadHostCaptureButton(TRUE, token, sizeof(token));

        for (;;)
        {
            const S32 left = 8 - (S32)iTimeDiffSec(start, iTimeGet());
            if (left <= 0)
            {
                return false;
            }

            char help[160];
            snprintf(help, sizeof(help), "Press a %s for %s.{n}Esc to cancel (%d).",
                     pad ? "controller button" : "key",
                     row < kBindLabelCount ? kBindLabels[row] : kPadBindButtons[row].name,
                     (int)left);
            DrawHelp(help);

            zSaveLoad_Tick();

            // Escape cancels on either page, and so cannot be bound here.
            bool gotKey = iPadHostCaptureKey(FALSE, token, sizeof(token)) != 0;
            if (gotKey && strcmp(token, "escape") == 0)
            {
                return false;
            }
            if (!pad && gotKey)
            {
                break;
            }
            if (pad && iPadHostCaptureButton(FALSE, token, sizeof(token)))
            {
                break;
            }
        }

        char key[64];
        BindKey(pad, row, key, sizeof(key));
        iConfigSet(key, token);
        iPadHostReloadBindings();
        Send("MNU4 MOVE B SFX", eEventPlay);
        return true;
    }

    void RunBindPage(bool pad)
    {
        S32 sel = 0;
        S32 top = 0;

        for (S32 r = 0; r < ISETTINGS_ROWS; r++)
        {
            SelectRow(r, false);
        }
        SelectRow(0, true);
        DrawBindPage(pad, sel, top, NULL);

        for (;;)
        {
            zSaveLoad_Tick();
            const U32 pressed = mPad[globals.currentActivePad].pressed;

            if (pressed & XPAD_BUTTON_TRIANGLE)
            {
                Send("MNU4 DENY SFX", eEventPlay);
                break;
            }

            S32 move = 0;
            if (pressed & XPAD_BUTTON_UP)
            {
                move = -1;
            }
            else if (pressed & XPAD_BUTTON_DOWN)
            {
                move = 1;
            }
            if (move != 0 && sel + move >= 0 && sel + move < BindRows())
            {
                SelectRow(sel - top, false);
                sel += move;
                if (sel < top)
                {
                    top = sel;
                }
                else if (sel >= top + ISETTINGS_ROWS)
                {
                    top = sel - ISETTINGS_ROWS + 1;
                }
                SelectRow(sel - top, true);
                DrawBindPage(pad, sel, top, NULL);
            }

            if (pressed & XPAD_BUTTON_X)
            {
                if (sel == kPadBindButtonCount)
                {
                    for (S32 i = 0; i < kPadBindButtonCount; i++)
                    {
                        char key[64];
                        BindKey(pad, i, key, sizeof(key));
                        iConfigUnset(key);
                    }
                    iPadHostReloadBindings();
                    Send("MNU4 MOVE B SFX", eEventPlay);
                }
                else
                {
                    Capture(pad, sel);
                }
                DrawBindPage(pad, sel, top, NULL);
            }
            else if ((pressed & XPAD_BUTTON_O) && sel < kPadBindButtonCount && MenuNeeds(sel))
            {
                Send("MNU4 DENY SFX", eEventPlay);
            }
            else if ((pressed & XPAD_BUTTON_O) && sel < kPadBindButtonCount)
            {
                // `none` rather than an empty value: bfbb_config reads an empty
                // line as "use the default". See iPadBind.h.
                char key[64];
                BindKey(pad, sel, key, sizeof(key));
                iConfigSet(key, "none");
                iPadHostReloadBindings();
                Send("MNU4 MOVE B SFX", eEventPlay);
                DrawBindPage(pad, sel, top, NULL);
            }
        }

        // Back to the list the page was opened from.
        SelectRow(sel - top, false);
        SelectRow(sSel - sTop, true);
        Draw();
    }

    // Left and right stop at the ends; X goes round.
    void Change(S32 dir, bool wrap)
    {
        const Setting& s = Selected();
        if (IsBindEntry(s))
        {
            // Only Confirm opens a page; left and right have nothing to move.
            if (wrap)
            {
                RunBindPage(strcmp(s.key, "bind.pad") == 0);
            }
            return;
        }
        const S32 n = WordCount(Words(s));
        const S32 was = IndexOf(s);

        S32 to = (was < 0 ? 0 : was + dir);
        if (wrap)
        {
            to = (to + n) % n;
        }
        if (to < 0 || to >= n || to == was)
        {
            return;
        }

        char before[64];
        Current(s, before, sizeof(before));
        char word[64];
        Word(Words(s), to, word, sizeof(word));

        Send("MNU4 MOVE B SFX", eEventPlay);
        Store(s, word);

        bool live = s.when != RESTART && s.apply != NULL;
        if (live)
        {
            s.apply(word);

            // A window the renderer could not switch -- Vulkan's exclusive
            // fullscreen is set at startup -- is kept as chosen for next time.
            if (strcmp(s.key, "video.mode") == 0 && iWindowGetMode() != ModeFor(word))
            {
                live = false;
                sRestart = true;
            }
        }
        else if (s.when == RESTART)
        {
            sRestart = true;
        }

        Draw();

        if (live && s.confirm && !Confirm())
        {
            s.apply(before);
            Store(s, before);
            Draw();
        }
    }

    void Show(S32 fromPause)
    {
        if (fromPause)
        {
            Send("PAUSE OPTIONS GROUP", eEventUIFocusOff);
            Send("PAUSE OPTIONS GROUP", eEventInvisible);
        }
        else
        {
            Send("MNU3 START GROUP", eEventUIFocusOff);
            Send("MNU3 START GROUP", eEventInvisible);
            Send("MNU3 BLUE ALPHA 1 UI", eEventVisible);
        }

        Send(ISETTINGS_GROUP, eEventVisible);
        Send(ISETTINGS_GROUP, eEventUIFocusOn);
        for (S32 row = 0; row < ISETTINGS_ROWS; row++)
        {
            SelectRow(row, false);
        }
        SelectRow(sSel - sTop, true);
        Draw();
    }

    void Hide(S32 fromPause)
    {
        Send(ISETTINGS_GROUP, eEventUIFocusOff_Unselect);
        Send(ISETTINGS_GROUP, eEventInvisible);

        if (fromPause)
        {
            // As retail's save screen puts the pause menu back.
            gGameMode = eGameMode_Pause;
            Send("PAUSE OPTIONS GROUP", eEventVisible);
            Send("PAUSE OPTIONS BKG GROUP", eEventUIFocusOn_Select);
            Send("PAUSE OPTIONS GROUP", eEventUIFocusOn);
            Send("PAUSE OPTION MGR UIF", eEventUIFocusOn_Select);
            Send(ISETTINGS_PAUSE_ENTRY, eEventUIFocusOn_Select);
        }
        else
        {
            Send("MNU3 START GROUP", eEventVisible);
            Send("MNU3 START GROUP", eEventUIFocusOn);
            Send(ISETTINGS_TITLE_ENTRY, eEventUISelect);
        }
    }
} // namespace

S32 iSettingsRequested(S32 fromPause)
{
    _zUI* entry = (_zUI*)zSceneFindObject(
        xStrHash(fromPause ? ISETTINGS_PAUSE_ENTRY : ISETTINGS_TITLE_ENTRY));
    return entry != NULL && (entry->uiFlags & 2) != 0;
}

void iSettingsRun(S32 fromPause)
{
    BuildResolutions();
    sTab = TAB_DISPLAY;
    sHoverTab = -1;
    BuildList();
    sSel = 0;
    sTop = 0;
    sRestart = false;

    Show(fromPause);
    iMenuMouseOwn(TRUE);

    for (;;)
    {
        zSaveLoad_Tick();
        const U32 pressed = mPad[globals.currentActivePad].pressed;

        if (pressed & XPAD_BUTTON_TRIANGLE)
        {
            Send("MNU4 DENY SFX", eEventPlay);
            break;
        }

        S32 move = 0;
        if (pressed & XPAD_BUTTON_UP)
        {
            move = -1;
        }
        else if (pressed & XPAD_BUTTON_DOWN)
        {
            move = 1;
        }
        if (move != 0 && sSel + move >= 0 && sSel + move < sListCount)
        {
            SelectRow(sSel - sTop, false);
            sSel += move;
            if (sSel < sTop)
            {
                sTop = sSel;
            }
            else if (sSel >= sTop + ISETTINGS_ROWS)
            {
                sTop = sSel - ISETTINGS_ROWS + 1;
            }
            SelectRow(sSel - sTop, true);
            Draw();
        }

        // The pointer selects the row it moves onto, and a click on a row is
        // the accept button on it, pressed next frame.
        const S32 pointed = PointerRow();
        if (pointed >= 0 && (iMenuMouseMoved() || iMenuMouseClicked()))
        {
            if (sTop + pointed != sSel)
            {
                SelectRow(sSel - sTop, false);
                sSel = sTop + pointed;
                SelectRow(pointed, true);
                Draw();
            }
            if (iMenuMouseClicked())
            {
                iMenuMouseAccept();
            }
        }

        // L1 and R1 turn the page, and go round.
        S32 page = 0;
        if (pressed & XPAD_BUTTON_L1)
        {
            page = -1;
        }
        else if (pressed & XPAD_BUTTON_R1)
        {
            page = 1;
        }
        S32 tab = (sTab + page + TAB_COUNT) % TAB_COUNT;

        // Or a click on the tab bar: a name, or the picture of the shoulder
        // button that turns that way. Asked every frame, which is what shows
        // the glove over the bar.
        const S32 pointedTab = PointerTab();
        if (pointedTab != sHoverTab)
        {
            sHoverTab = pointedTab;
            Draw();
        }
        if (page == 0 && iMenuMouseClicked() && pointedTab >= 0 && pointedTab != sTab)
        {
            tab = pointedTab;
            page = 1;
        }

        if (page != 0)
        {
            SelectRow(sSel - sTop, false);
            sTab = (Tab)tab;
            BuildList();
            sSel = 0;
            sTop = 0;
            // Selecting the row plays the turn's sound: its link's.
            SelectRow(0, true);
            Draw();
            continue;
        }

        if (pressed & XPAD_BUTTON_LEFT)
        {
            Change(-1, false);
        }
        else if (pressed & XPAD_BUTTON_RIGHT)
        {
            Change(1, false);
        }
        else if (pressed & XPAD_BUTTON_X)
        {
            Change(1, true);
        }
    }

    iMenuMouseOwn(FALSE);
    iConfigSave();
    Hide(fromPause);
}
