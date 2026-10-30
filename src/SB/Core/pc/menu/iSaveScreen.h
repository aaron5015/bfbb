#ifndef ISAVESCREEN_H
#define ISAVESCREEN_H

// The PC save and load screens' widgets, as the menu data builds them and
// iSaveScreen.cpp drives them. One list of every save, newest first, shown a
// few rows at a time.

#define ISAVESCREEN_ROWS 5

// The rows' names, "%d" the row. The text is shared: the two screens are never
// up at once.
#define ISAVESCREEN_LOAD_ROW "PC LD ROW %d"
#define ISAVESCREEN_SAVE_ROW "PC SV ROW %d"
#define ISAVESCREEN_ROW_TEXT "PC SAVE ROW %d TXT"

// The pause menu's Load Game entry: retail's, unused until now. It opens the
// save screen's widgets as a list of saves to load.
#define ISAVESCREEN_PAUSE_LOAD "PAUSE OPTION LOAD UIF"

// The save screen's overwrite question, which loading from the pause menu
// rewrites to ask about the unsaved game instead.
#define ISAVESCREEN_OVERWRITE_TEXT "SV OVERWRITE TXT"
#define ISAVESCREEN_OVERWRITE_ASK "A game already exists. Would you like to overwrite it?"

// The save screen's failure message, which a failed load from the pause menu
// rewrites: the title's load-failed message is in mnu3, not loaded in a level.
#define ISAVESCREEN_BADSAVE_TEXT "SV BADSAVE TXT"
#define ISAVESCREEN_BADSAVE_SAY                                                                    \
    "{c=ffff0000}Save failed!{~:c} There is not enough free space to create new save games. "      \
    "Press {i:ui_accept} to continue."
#define ISAVESCREEN_BADLOAD_SAY                                                                    \
    "{c=ffff0000}Load failed!{~:c} Unable to load game. Press {i:ui_accept} to continue"

// Room for a row's two lines.
#define ISAVESCREEN_ROW_TEXT_SIZE 160

// Where the rows sit, in the 640x480 the menus are laid out in. Left of the
// thumbnail, which is at (430, 180) on both screens.
#define ISAVESCREEN_ROW_X 60.0f
#define ISAVESCREEN_ROW_Y 118.0f
#define ISAVESCREEN_ROW_STEP 48.0f
#define ISAVESCREEN_ROW_W 360
#define ISAVESCREEN_ROW_H 46

#endif
