#ifndef ISETTINGSSCREEN_H
#define ISETTINGSSCREEN_H

#include <types.h>

// The in-game settings screen: config.ini's settings a player would change,
// changed from the pause menu or the title and applied as they change. The
// widgets are the menu data's (mnu4, which is loaded everywhere); the rest is
// iSettingsScreen.cpp.

#define ISETTINGS_ROWS 7

// Each row is a label and a value, "%d" the row.
#define ISETTINGS_LABEL "PC SET LABEL %d"
#define ISETTINGS_VALUE "PC SET VALUE %d"
#define ISETTINGS_LABEL_TEXT "PC SET LABEL %d TXT"
#define ISETTINGS_VALUE_TEXT "PC SET VALUE %d TXT"
#define ISETTINGS_TITLE "PC SET TITLE UIF"
#define ISETTINGS_TITLE_TEXT "PC SET TITLE TXT"
#define ISETTINGS_HELP "PC SET HELP UIF"
#define ISETTINGS_HELP_TEXT "PC SET HELP TXT"
#define ISETTINGS_GROUP "PC SETTINGS GROUP"

// "Back", beside the help line: a hint like the pause menu's, so the pointer
// can leave the screen as the cancel button does.
#define ISETTINGS_BACK "PC SET BACK UIF"
#define ISETTINGS_BACK_TEXT "PC SET BACK TXT"

// The scrollbar beside the rows: a faint track and the thumb on it, drawn
// only while a tab has more rows than fit.
#define ISETTINGS_SCROLL_TRACK "PC SET SCROLL TRACK UIF"
#define ISETTINGS_SCROLL_THUMB "PC SET SCROLL THUMB UIF"

// The retail Options page's arrows either side of each row's value, and their
// glow for the one under the pointer. "%d" is the row.
#define ISETTINGS_ARROW_LEFT "PC SET ARROW %d LEFT"
#define ISETTINGS_ARROW_RIGHT "PC SET ARROW %d RIGHT"
#define ISETTINGS_ARROW_LEFT_GLOW "PC SET ARROW %d LEFT GLOW"
#define ISETTINGS_ARROW_RIGHT_GLOW "PC SET ARROW %d RIGHT GLOW"
#define ISETTINGS_ARROW_SIZE 20

// The volume sliders, in the same page's bamboo: a notch per level from 0 to
// 10, yellow up to the level and plain past it, between the row's arrows. Two
// sets, which is as many slider rows as a tab shows; each is placed on its row
// as the screen is drawn. "%d" is the set, then the notch.
#define ISETTINGS_SLIDERS 2
#define ISETTINGS_SLIDER_LEVELS 11
#define ISETTINGS_SLIDER_ON "PC SET SLIDER %d ON %d"
#define ISETTINGS_SLIDER_OFF "PC SET SLIDER %d OFF %d"

// The menu entries that open it.
#define ISETTINGS_TITLE_ENTRY "MNU3 START SETTINGS UIF"
#define ISETTINGS_PAUSE_ENTRY "PAUSE OPTION SETTINGS UIF"

#define ISETTINGS_ROW_Y 105.0f
#define ISETTINGS_ROW_STEP 34.0f

// Whether the entry that opens the screen was just chosen. Asked by the save
// and load loops, which the entries reach through the game's own save and
// load modes: the title's goes through Load, the pause menu's through Save.
S32 iSettingsRequested(S32 fromPause);

// Run the screen until the player backs out, then put back the menu it came
// from.
void iSettingsRun(S32 fromPause);

// What the retail Options page set -- the volumes, the sound mode and
// vibration -- which retail kept in each save file and the PC keeps in
// config.ini, one for the install. Levels are 0 to 10, as the page showed them.
void iSettingsApplyMusicVolume(S32 level);
void iSettingsApplySFXVolume(S32 level);
void iSettingsApplySoundMode(S32 stereo);
void iSettingsApplyVibration(S32 on, S32 pulse);

// The three sound ones from config.ini, once the sound system is up.
void iSettingsApplyAudio();

#endif
