#ifndef IANDROIDGAMEMAIN_H
#define IANDROIDGAMEMAIN_H

// Renames the game's entry point, and is FORCE-INCLUDED into zMain.cpp on
// Android only -- see the android section of CMakeLists.txt, which passes it
// with -include.
//
// Android has no process entry point to give a game. The application is
// started by the Java framework, which loads a shared object and calls an
// exported SDL_main on a thread of its own; `main` in a .so is never called
// by anything. SDL's own header spells that as `#define main SDL_main`, which
// would work here too, except that it leaves nowhere to put the Android
// startup that has to happen before the game's first line. So the game's main
// becomes an ordinary function instead, and iAndroidMain.cpp supplies the
// SDL_main that does the setting up and then calls it.
//
// zMain.cpp is not touched. Its main() is retail's, shared with the GameCube
// build, and the one thing that must not acquire a host #ifdef.
//
// extern "C" is load-bearing twice over: the definition in zMain.cpp picks up
// this declaration's linkage, so the symbol is not mangled, and iAndroidMain.
// cpp can then name it.

extern "C" int bfbbGameMain(int argc, char** argv);

#define main bfbbGameMain

#endif
