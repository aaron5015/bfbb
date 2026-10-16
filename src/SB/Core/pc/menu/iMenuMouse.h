#ifndef IMENUMOUSE_H
#define IMENUMOUSE_H

#include <types.h>

struct xBase;

// PC-only: the mouse in the game's menus. While a menu is up the pointer is
// shown as the game's own, moving it over an option selects that option, and
// a left click accepts it.
//
// **Selection goes through the menu's own links.** Each option says, in its
// links, which neighbour a pad press moves the highlight to. Hovering finds
// the shortest chain of those presses from the option that is selected now to
// the one under the pointer and sends them, so every sound, every unselect and
// every manager-side effect is the one the pad would have made. An option no
// chain reaches is not one a pad could select either, and is left alone.
//
// **A click is the accept button.** It adds XPAD_BUTTON_X to the frame's pad
// (START for an option that only answers START), so it reaches every reader
// of the pad: zUI's links, the screens that read uiButton directly, the save
// prompts and the port's own screens. Clicking where no option is does nothing,
// which keeps a click from skipping anything.
//
// **A hint is a button.** Text that leads with a button picture -- "{i:ui_options}
// Options", "{i:ui_cancel} Return to Game" -- presses that button when clicked,
// where no option is under the pointer.
//
// The settings and save screens keep their own selection in code rather than
// in links. They ask iMenuMouseOver about their rows and do their own moving.

// iPad.cpp's menu hook; see iPad.h. Registered by iSystem.cpp.
U32 iMenuMouseUpdate(S32 inMenu);

// Whether the pointer is over `ent`, a zUI or zUIFont, this frame. FALSE when
// no pointer is showing or `ent` is not visible.
S32 iMenuMouseOver(xBase* ent);

// Whether the pointer moved, and whether the left button went down, this
// frame. Both FALSE when no pointer is showing.
S32 iMenuMouseMoved();
S32 iMenuMouseClicked();

// The pointer in the menus' 640x480 units. FALSE when none is showing.
S32 iMenuMousePosition(F32* x, F32* y);

// Where, across the menus' 640 units, the words of `prefix` end when `ent` (a
// zUIFont) draws its whole text: `prefix` is the start of that text. For one
// string that is several targets, such as the settings screen's tab bar.
S32 iMenuMouseTextEnd(xBase* ent, const char* prefix, F32* x);

// For a screen that keeps its own selection: while owned, hovering and
// clicking do nothing on their own and the screen moves its selection itself.
// Its rows have links as well, which the selection in code would disagree with.
void iMenuMouseOwn(S32 on);

// Presses accept on the next frame, once the screen has moved its selection
// onto the row that was clicked, so the row's own links fire as from a pad.
void iMenuMouseAccept();

#endif
