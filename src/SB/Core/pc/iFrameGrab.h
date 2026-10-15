#ifndef IFRAMEGRAB_H
#define IFRAMEGRAB_H

#include <types.h>

// PC-only. Writes the frame about to be presented to `path` as a PNG,
// area-averaged down to at most `maxWidth` pixels wide; 0 keeps the full size.
// FALSE when there is no frame to read or the file cannot be written. Call it
// from iSnapshotCapture's frame hook, which runs before the flip.
//
// For iTour.h. Reads the game's own render target, never the desktop.
S32 iFrameGrabWrite(const char* path, S32 maxWidth);

// The same frame as packed RGB, top row first, `*outWidth` x `*outHeight`;
// the caller frees it. NULL when there is no frame to read. For a recording
// tour, which takes one of these every frame.
U8* iFrameGrabRGB(S32 maxWidth, S32* outWidth, S32* outHeight);

#endif
