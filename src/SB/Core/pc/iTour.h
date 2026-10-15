#ifndef ITOUR_H
#define ITOUR_H

#include <types.h>

// PC-only. Runs the game from a script, in a hidden window, and writes PNGs of
// its own frames: a way to look at the menus without a person at the machine.
//
//   BFBB_TOUR=path/to/script.txt  the script; tour mode is on when this is set
//   BFBB_TOUR_OUT=dir             where the PNGs go; the script's folder if unset
//
// Port 0 reads the script and nothing else; real controllers and the keyboard
// are ignored. The window is created hidden and stays windowed, so the render
// size is video.width x video.height. The game's clock advances exactly 1/60
// of a second per frame (iTimeFixedStep), so counts below are seconds / 60 and
// a script plays the same way each run. Nothing caps the frame rate: a tour
// runs as fast as the machine draws. Nothing is played aloud; the voices still
// play out on the same clock (iSndHostSDL.cpp), and a recording mixes them.
//
// One command a line; `#` starts a comment. Counts are in presented frames.
//
//   wait N              do nothing for N frames
//   press B [B...]      hold the buttons 4 frames, then release for 4
//   hold N B [B...]     hold the buttons N frames, then release for 4
//   move N DEG[@MAG] [B...]
//                       hold the left stick at DEG (0 up, 90 right) for N
//                       frames, at MAG (default 1), with the buttons held too.
//                       No release: consecutive moves are one continuous run
//   cam N DEG[@MAG]     the same on the right stick, which turns the camera
//   goto X Z [TOL [MAXFRAMES]] [B...]
//                       steer the left stick toward world (X, Z) until the
//                       player is within TOL (default 1.5) of it, for at most
//                       MAXFRAMES (default 1800), with the buttons held
//
// In move, cam and goto, a button written *B is tapped instead of held: down 4
// frames of every 12.
//   shot NAME [WIDTH]   write NAME.png, at most WIDTH wide (default 1280; 0
//                       keeps the render size)
//   record NAME [WIDTH] from this frame on, write every frame and the sound
//                       to NAME.mp4 through ffmpeg (BFBB_FFMPEG, else ffmpeg
//                       on PATH). WIDTH as for shot; default the render size
//   stop                finish the recording (the end of the script does too)
//   pos                 print the frame number, the player's position and
//                       facing, and the camera's facing
//   quit                exit the process
//
// Buttons are the [pad] names from config.ini: a b x y z hud l1 r1 l2 r2 start
// select up down left right. The process exits when the script ends.
void iTourInit();

// Whether BFBB_TOUR is set. iWindowSDL.cpp reads the variable itself, since
// the shim does not link the platform layer.
S32 iTourActive();

#endif
