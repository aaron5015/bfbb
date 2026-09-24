#ifndef ITOUCHOVERLAY_H
#define ITOUCHOVERLAY_H

#include <types.h>

// PC-only. The shapes the on-screen controls are drawn with, handed from the
// input side (iPadTouch.cpp, in the platform layer) to the renderer
// (rw/touch_overlay.cpp, in the RenderWare shim). The renderer cannot call the
// platform layer, so the controls are pushed down to it rather than pulled.
//
// Drawn over the finished frame at the window's resolution, after the game's
// picture is scaled into the window, so a control can sit in the bars beside a
// 4:3 frame. GL3 only; on another backend nothing draws the shapes.
//
// The storage is iTouchOverlay.cpp, which depends on nothing, so the input side
// links into pc_selftest without the renderer.

// A rounded rectangle, a circle when `corner` is at least both half extents.
// Window pixels, y down.
struct iTouchOverlayShape
{
    F32 x;
    F32 y;
    F32 halfWidth;
    F32 halfHeight;
    F32 corner;

    // 0 fills the shape; otherwise the width of an outline along its edge.
    F32 stroke;

    U8 r;
    U8 g;
    U8 b;
    U8 a;

    // Drawn centred in white, or nothing when 0. A-Z and '>' (start) and '='
    // (select); see kGlyphs in rw/touch_overlay.cpp.
    char label;
};

#define ITOUCHOVERLAY_MAX_SHAPES 64

// Replaces what the next present draws. `count` 0 draws nothing.
void iTouchOverlaySubmit(const iTouchOverlayShape* shapes, S32 count);

// The window size the last present drew at, in the same pixels as the shapes.
// False before the first present on a backend that draws the overlay.
bool iTouchOverlayWindowSize(S32* width, S32* height);

// Hooks the drawing into librw's present. Called once the GL3 device is open;
// defined in rw/touch_overlay.cpp.
void iTouchOverlayInstall();

// For the renderer: what was last submitted, and the size it drew at.
S32 iTouchOverlayShapes(const iTouchOverlayShape** shapes);
void iTouchOverlaySetWindowSize(S32 width, S32 height);

#endif
