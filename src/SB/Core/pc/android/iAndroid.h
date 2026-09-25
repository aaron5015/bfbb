#ifndef IANDROID_H
#define IANDROID_H

// The part of starting up that only Android has.
//
// Everything else in src/SB/Core/pc is written against a host that has a
// console, an executable in a directory, and a working directory the player
// chose. Android has none of the three: the process image is the zygote, the
// application's files live in a directory whose name contains the package and
// the user id, and stdout goes nowhere at all. What is below papers over
// exactly those three and nothing else -- the host seam (iHost.h) is where
// the rest of the difference is absorbed, and it reads what these functions
// put in the environment.
//
// Two entry points because there are two moments, and they are not the same
// moment:
//
//   iAndroidOpenLog runs from a static constructor, which is to say during
//   dlopen, BEFORE the JVM has called JNI_OnLoad. Nothing about the
//   application is knowable yet -- not its package, not its directories, not
//   even that there is an Activity -- so this half is pure POSIX and touches
//   only file descriptors.
//
//   iAndroidStartup runs at the top of SDL_main, by which time SDL's JNI
//   plumbing is up and the Activity can be asked where its directories are.
//
// Getting them the wrong way round is not a warning: SDL_GetAndroid*Path
// before JNI_OnLoad returns NULL, and the port would then write its config
// into a directory that does not exist and report no assets.

// stdout and stderr to logcat.
//
// Android discards both. The platform layer prints from about thirty files
// and the startup banner is printed before anything is on screen, so without
// this every diagnosis the port has -- which stub was reached, which texture
// did not convert, which line came last before the hang -- is thrown away.
// This is therefore the FIRST thing that happens in the process, ahead of the
// banner in bfbb_main.cpp, because a message printed before it is gone.
//
// Lines come out under the tag "bfbb" at INFO, so `adb logcat -s bfbb` is the
// whole log and nothing else.
//
// They ALSO go to bfbb-log.txt in the external files directory, which is the
// same place the assets are imported to and is openable from any file manager
// on the device. logcat needs adb, which needs a computer or a phone paired
// with itself; a player who says "it closed instantly" has neither, and that
// is the report this port is going to get. Lines printed before JNI is up --
// which is all of the earliest and most important ones -- are held in memory
// until iAndroidStartup knows where the file goes.
void iAndroidOpenLog();

// Where the application's directories are, into the environment, for the host
// seam to read back: BFBB_ANDROID_INTERNAL, BFBB_ANDROID_CACHE and
// BFBB_ANDROID_EXTERNAL. Also points BFBB_ASSETS at the imported assets when
// they are there and nothing has already named somewhere else.
void iAndroidStartup();

#endif
