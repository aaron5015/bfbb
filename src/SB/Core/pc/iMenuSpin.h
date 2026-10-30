#ifndef IMENUSPIN_H
#define IMENUSPIN_H

#include <types.h>

// PC-only: the pause menu's task spatulas, kept spinning in step.
//
// Each task row on the level browser is one of 16 UI models: eight gold
// spatulas and eight silver ones, shared by every world's page. zUI_Update
// advances a UI model's animation only while it is visible, so a spatula that
// one world's page hides stops where it is, and comes back on the next page
// that shows it behind the ones that kept turning. Paging between worlds with
// different tasks done leaves every spatula at a different point in its spin.
// See iFixes.h for the switch.

struct xEnt;

// Give every model in `ents` the animation time of the first one that is
// visible, and re-evaluate the visible ones that moved. Called once a frame
// after the page has decided what is shown, which is after the UI update, so
// a spatula shown this frame is posed here rather than one frame late. Models
// whose animation runs for a different length are left alone. Nothing happens
// with fixes.spatula_spin off.
void iMenuSpinSync(xEnt* const* ents, S32 count);

#endif
