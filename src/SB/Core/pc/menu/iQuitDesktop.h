#ifndef IQUITDESKTOP_H
#define IQUITDESKTOP_H

#include <types.h>

// The pause menu's Quit Game asks where to: Quit to Main Menu (retail's Yes),
// Quit to Desktop (this), or Cancel (retail's No). The widgets are the menu
// data's (mnu4). Quit to Desktop switches the game to Save mode and stays
// selected, the way the Settings entry does, and iSGSaveLoop sees that and
// closes the window.
#define IQUIT_DESKTOP_YES "PC QUIT DESKTOP YES UIF"

// Where the three choices sit, in the 640x480 the menus are laid out in: one
// column, centred, wide enough for "Quit to Main Menu".
#define IQUIT_CHOICE_X 120.0f
#define IQUIT_CHOICE_W 400

// Whether Quit to Desktop was just chosen. Asked by the save loop.
S32 iQuitDesktopRequested();

#endif
