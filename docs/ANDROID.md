# The Android port

The PC port, on a phone. This is the plan and the current state of it.

Nothing here has been built with an NDK or run on a device. Everything below
that says "works" means it compiles in the tree it belongs to and follows from
reading the code; everything that says "unverified" means exactly that. The
first two phases are largely written; the questions that decide whether the
port is possible at all are still open, and are listed under **What is still
unknown** at the end.

---

## The short version

It is closer than "GameCube game on a phone" sounds. The seam is host-shaped
rather than Windows-shaped, and the renderer already has a GLES arm. Two
things cannot be hand-waved; everything else is build plumbing, touch input,
and a way to get the assets onto the device.

## What is already in your favour

`src/SB/Core/pc/iHost.h` is the whole OS surface, and it is 217 lines.
`iHostPosix.cpp` says in its own comment that "a third host wanting in adds its
arm the same way", and that is what Android is now: the same file, with
`__ANDROID__` arms beside the `__APPLE__` ones. Every other seam --
`iPadHost`, `iSndHost`, `iFMVAudio`, `iWindow` -- has a Null arm beside the
real one, so each can be brought up empty and filled in later.

**librw already has GLES.** `gl3device.cpp` carries `shaderDecl310es` and
`shaderDecl100es`, picked from a runtime `gl3Caps.gles` flag, with
`gladLoadGLES2Loader` behind it. The port's own post-process shaders inherit it
free: `rw/glow.cpp` builds from `rw::gl3::shaderDecl` plus a `.frag`.

**Textures are not a problem.** `rw/texture.cpp` already runs
`convertRasterToPlatform` over every TXD, and `xbox_to_gl3` is real. That is
how the Linux and macOS GL3 builds draw at all.

**AArch64 already compiles.** `compat/intrin.h` has the `fsqrt` arm for
`__frsqrte`, and CI's `macos-latest` is Apple Silicon. (32-bit ARM did not
have an arm and now does; see **The low 4 GB** below for why that matters.)

**Audio is already SDL3** (`iSndHostSDL.cpp`), which has an Android backend.

**Byte order is a non-issue** in both directions.

---

## Problem 1: the low 4 GB

`iMemMgr.cpp` refuses to start if the game arena lands above 4 GB:
`gMemInfo.DRAM.addr` is a `U32` and `xMemInitHeap` does pointer arithmetic on
it as an integer, so every address the game allocator hands out has to survive
the round trip back to a pointer. `iHostReserveLow` gets the arena there with
`MAP_32BIT` on x86-64, and otherwise by walking `mmap` hints from `0x04000000`
up in 64 MB steps.

This is what killed Apple Silicon: arm64 macOS requires a 4 GB `__PAGEZERO`
and SIGKILLs a smaller one, so there is no low 4 GB to map into and the port
dies in `iMemInit` saying so. **Android arm64 has no such rule**, so the hint
loop ought to work -- subject to `vm.mmap_min_addr` and to the low region
actually being free under the device's ASLR. Neither is verified, and it is a
twenty-line NDK program to find out.

`tools/android/arena_probe.c` is that program. Run it first, before writing
anything else:

```
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang \
    -O2 -o arena_probe tools/android/arena_probe.c
adb push arena_probe /data/local/tmp/ && adb shell chmod 755 /data/local/tmp/arena_probe
adb shell /data/local/tmp/arena_probe
```

It runs the same loop `iHostReserveLow` runs, asks for the same 128 MB
`iMemMgr` asks for, writes to both ends of what it gets, and exits non-zero if
nothing below 4 GB was free. One caveat is in its header comment and is worth
repeating: it is a bare executable run from a shell, while the game is a
library inside a zygote fork with a JVM, ART's heaps, SDL and the graphics
driver already mapped. **A failure settles the question; a pass only makes it
worth trying.** The real confirmation is the port reaching `iMemInit` on a
device and not printing "outside the low 4 GB".

If it fails, **armeabi-v7a is a genuinely appealing fallback**: four-byte
pointers natively, which sidesteps every caveat under "Asset caveats" in
[PCPORT.md](PCPORT.md) rather than relying on where the arena happens to land.
The cost is that Google Play requires a 64-bit binary and that 32-bit ARM is
absent from new silicon. It is one line in `android/app/build.gradle`
(`abiFilters`). `compat/intrin.h` has an `armeabi-v7a` arm for `__frsqrte` now
so that the fallback does not quietly land in the generic `__builtin_sqrt`
case, which carries a recursion hazard its own comment describes.

Two changes went in alongside the probe:

* `iHostReserveLow` now passes `MAP_FIXED_NOREPLACE` where the kernel has it
  (Linux 4.17 and up, which is every Android that can run this). A plain hint
  may be answered with any address at all, and a kernel that declines the
  first hint tends to decline all 31 the same way -- handing back the same
  high mapping each time, which the loop then unmaps to find out. With the
  flag the mapping either lands exactly where it was asked for or fails. An
  older kernel ignores the flag, which leaves the behaviour that was there
  before, so the result is still checked rather than assumed.
* `iMemMgr`'s refusal is unchanged and should stay that way. It is the thing
  that turns this from silent corruption into a sentence.

---

## Problem 2: losing the GL context

Android may destroy the EGL context when the activity stops. Every raster,
VBO, program and render target in librw goes invalid, and **librw has no
rebuild path** -- there is no GL3 device-reset handler and nothing calls one.
On a desktop this is a fullscreen edge case; on Android it is ordinary
lifecycle, which is to say a phone call.

There are three answers:

1. **Hope.** Surface hints plus pausing the loop. Cheapest, and wrong on some
   devices.
2. **Reload the scene from the asset system on loss.** The cheapest honest
   answer, and it costs a loading screen every phone call.
3. **Add re-instancing to the librw fork.** The right answer, and its own
   project.

Decide before starting on rendering, because it changes whether rasters must
retain their source pixels.

What is in the tree so far is only the part that keeps the problem from being
worse than it has to be: `android:configChanges` in the manifest lists every
configuration change the activity handles itself, so a rotation or a keyboard
appearing does not destroy and recreate the activity -- which would take the
context out from under the game for a reason that has nothing to do with the
app being backgrounded.

---

## Everything else, by size

### librw's GLES arm

Three concrete items. The first and third are done; the second turned out to
be smaller than it looked.

**The profile loop, fixed.** `startSDL3` broke out of its loop on the first
successful `SDL_CreateWindow` and created the context *after* the loop. A
window is created from attributes alone and does not fail on a profile the
driver cannot give -- so the loop always took the first entry, CORE 3.3, and
the two ES entries below it were unreachable. On Android that is a context
creation failure with the working profiles never tried. Window, context and
the glad load now succeed or fail together, per profile, and a profile that
fails any of the three falls through to the next. It was a defect on desktop
too. The same shape is in `startSDL2` and `startGLFW`; nothing in this project
compiles those arms, so they were left alone.

**S3TC, fixed, and not the way the original audit proposed.** Adreno and Mali
expose ETC and ASTC and no S3TC at all, so `gl3Caps.dxtSupported` is false on
most phones. The audit's plan was to decompress DXT on the CPU when the
extension is absent -- but **librw already does that**:
`Raster::convertTexToCurrentPlatform` falls through to an Image path that calls
`decompressDXT1/3/5`, which is what the `dxt == 0` case has always used. The
actual defect was narrower: `d3d_to_gl3` checks `dxtSupported` and returns nil,
and `xbox_to_gl3` beside it did not. Without the check it built a DXT raster
that could not be uploaded, and the failure landed at
`glCompressedTexImage2D` with the texture already installed -- so the surface
drew as garbage rather than as an unsupported format. Adding the same check
sends it down the Image path instead. Blast radius either way is bounded and
known: db05 is the only level shipping compressed textures, 55 of them.

Orientation was the thing to check before making that change, and it holds:
both paths flip. `gl3raster.cpp`'s `rasterFromImage` walks the image from its
last row backwards, because the GL3 fragment shaders sample `1.0 - v`; the DXT
path calls `flipDXT`, which does the same job to whole blocks. See
`src/SB/Core/pc/iRasterFill.h`, which documents the convention for the port's
own hand-filled rasters.

**Render targets and readback.** Flagged GLES-hostile by librw's own comments
in `gl3raster.cpp`. Everything the port draws goes through `setVirtualScreen`,
and glow, distort and snapshot each capture the back buffer. These need a
device, one at a time; the snapshot readback is the most suspect. Nothing has
been done here and nothing should be until something draws.

**One more, found on the way:** librw's `find_package(OpenGL)` fails outright
on Android -- the NDK sysroot has no `GL/gl.h` -- and there is nothing there
for it to link anyway. glad holds every entry point as a function pointer and
fills them from `SDL_GL_GetProcAddress`, so librw refers to no GL symbol at
link time. The Android arm skips the lookup.

All four changes are on the `librw-android` branch of this repository, not on
the `librw` fork; `.gitmodules` points `third_party/librw` there.

### The entry point

`main` is retail source, at `zMain.cpp:126`, and it is shared with the
GameCube build -- the last file that should learn about a host.

Android does not call `main` at all. `SDLActivity` loads a shared object,
`dlsym`s `SDL_main` out of it, and runs that on a thread of its own. So:

* the game builds as `libmain.so` rather than an executable -- CMake target
  `main`, which is the name `getMainSharedObject()` looks for;
* `zMain.cpp` is compiled with `-include android/iAndroidGameMain.h` on
  Android only, a three-line header that declares `extern "C" int
  bfbbGameMain(int, char**)` and `#define main bfbbGameMain`. No source
  change, GameCube arm untouched. (Verified: with the force-include the object
  exports `T bfbbGameMain`, unmangled; without it, `T main`.)
* `iAndroidMain.cpp` supplies the `SDL_main` that does the Android startup and
  then calls it.

SDL's own header spells this as `#define main SDL_main`, which would work too,
except that it leaves nowhere to put the setup that has to happen before the
game's first line.

The blocking `zGameLoop` is fine; SDL runs it on its own thread.

### The log

Android discards `stdout`. Thirty files in the platform layer print, and the
startup banner is printed before anything is on screen, so without a redirect
every diagnosis the port has -- which stub was reached, which texture did not
convert, which line came last before the hang -- is thrown away. **This is
what makes every later phase debuggable, so it is the first thing that runs**:
`iAndroidOpenLog` puts a pipe on fds 1 and 2 and pumps it into
`__android_log_write` a line at a time, from a static constructor in
`bfbb_main.cpp`, ahead of the banner.

`adb logcat -s bfbb` is then the whole log and nothing else.

`iHostPrintCallers` used to print nothing on bionic, which has no
`execinfo.h`. It now walks the stack with `_Unwind_Backtrace` and names frames
with `dladdr`. That matters more on a phone than on a desktop, not less: there
is no debugger attached and a fault otherwise ends as an address in a
tombstone.

### The host seam

Four functions have Android arms now, all in `iHostPosix.cpp` beside the
`__APPLE__` ones:

* `iHostExeDir` -- `/proc/self/exe` is `app_process64`, the zygote, shared by
  every app on the device. A `config.ini` written beside it would be written
  into `/system`. It answers with the app's internal storage directory
  instead, which is what the callers (`iConfig.cpp`) actually want: somewhere
  that ships with the application and is still there next time.
* `iHostUserDataDir` -- the same directory, deliberately. An Android app has
  one private directory and both questions resolve to it. Not the external
  one: saves there are visible to a file manager, which sounds helpful until
  an update or a "clear storage" tap takes them.
* `iHostTempDir` -- the app's cache directory. `/tmp` does not exist.
* `iHostRunDetached` -- false. There is no second executable to start, the
  caller is the settings front end, and the data partition is mounted
  `noexec`.
* `iHostErrorBox` -- `__android_log_print` at ERROR. Not a dialog: there is
  nothing to dismiss, and the header allows a host with no way to show one.
  It is still better than nothing, because on a phone a startup failure is a
  process that disappears without a word.

The seam does not talk to SDL to find those directories, and should not: it is
the OS half of the platform layer and SDL is a library on top of it. The
Android shim asks SDL once, at the top of `SDL_main`, and puts the answers in
the environment (`BFBB_ANDROID_INTERNAL`, `BFBB_ANDROID_CACHE`,
`BFBB_ANDROID_EXTERNAL`) through the `iHostSetEnv` that already existed. Unset
until then, and every reader returns false rather than guessing -- so an early
failure degrades to a relative path instead of writing somewhere that does not
exist.

### Assets

`fopen` on an arbitrary external path has not worked since API 30. The
application's own directory under `Android/data` is the only place both sides
can reach: no permission is needed from this side, and it is visible over USB
and to a file manager from the other.

So the assets go in
`Android/data/io.github.joeyballentine.bfbb/files/assets`, the activity
creates that directory on first run so the instruction is one someone can
follow, and `iAndroidStartup` points `BFBB_ASSETS` at it -- but only when the
directory exists and only when nothing has already set `BFBB_ASSETS`, so
`[assets] path` in `config.ini` still decides for a build that keeps its data
somewhere unusual and the documented precedence is not quietly inverted.

This is the copy-in-twice approach, and it costs twice the disk during the
copy. The alternative -- routing `iFile` through SAF file descriptors -- means
`iFile.cpp` and the `dirent` walk in `iHostPosix.cpp` both learn about a
handle that is not a path. Worth revisiting only if the copy proves
intolerable.

A **first-run import screen** is not written. Today the game reports no assets
and says where it looked, which is enough to act on and is not enough to ship.

### Input

Controllers work today with nothing written: SDL's Android backend reaches
them and `iPadHostSDL.cpp` is already the input backend. **Controller-only is
a legitimate first milestone.**

Touch has no arm at all. It needs a new `iPadHostTouch.cpp` plus an on-screen
overlay -- but `iPadStick`, `iPadLayout`, `iPadBind` and `iPadGlyph` are all
already generic, and `video.ui`'s edge anchoring is what lets a control be
placed against a real screen edge. Two sticks, four faces, two shoulders and a
d-pad is a lot of thumb; expect design work, with the bungee and the sliding
sections as the stress test.

### Frame pacing

The present is vsynced whether you ask or not, and the panel may be 90 or 120
Hz. `dt` is real frame time, and the uncapped audit fixed the systems it
names -- but [UNCAPPED.md](UNCAPPED.md) also lists what has not been swept.
Keep the 60 cap deciding the rate rather than the display.

### Switched off

* **The configurator.** `BFBB_BUILD_CONFIGURATOR` defaults off on Android. It
  is a wxWidgets window, and it exists to be run *before* the game, which is
  not something an Android app can do to itself. `config.ini` is still
  written with the defaults on first run, into internal storage, and can be
  pulled and pushed with `adb`. A settings screen inside the game is phase 6.
* **FFmpeg**, unless cross-compiled. Already optional; the NDK toolchain
  restricts `find_package` to the sysroot, so the host's copy is not picked
  up by accident.
* **`-m32` and the `CMAKE_SIZEOF_VOID_P` assertion.** These only fire when
  `BFBB_BUILD_32BIT` is on, which off Windows it is not -- but a Windows
  machine cross-building for a phone would have defaulted it on and died in
  the compiler probe. The default now accounts for an Android target, and
  asking for both is an error that says `abiFilters` instead.
* **The host-side executables.** `pc_selftest`, `rw_selftest`, `fps_selftest`
  and `fontfit` exist to be *run* by ctest on the build machine, and a cross
  build produces binaries it cannot run. `BFBB_HOST_TOOLS` is off on Android.
* **The heavy defaults** -- hipoly tessellation, per-pixel lighting,
  supersampling. Not done: these are `config.ini` values and want a phone
  profile, which is phase 6.

---

## Building it

```
cd android
gradle assembleDebug
```

There is no `gradlew` checked in. A Gradle wrapper is worth adding -- it is
what pins the Gradle version the way `ndkVersion` pins the toolchain -- but
generating one downloads the Android Gradle plugin, which the machine this was
written on could not reach. `gradle wrapper --gradle-version 8.9` from
`android/` is the whole of it.

Gradle invokes the port's own `CMakeLists.txt` three directories up through
`externalNativeBuild`; there is no second build system and no copy of
anything. It needs a JDK 17, the Android SDK, and the NDK version pinned in
`android/app/build.gradle`.

The submodules have to be there first, as for any other build:

```
git submodule update --init --recursive
```

SDL is the vendored one and is linked **statically** into `libmain.so`. That
is why `BfbbActivity.getLibraries()` returns `{"main"}` and not SDL's default
`{"SDL3", "main"}` -- there is no second library, and asking for one is an
`UnsatisfiedLinkError` before any of the port runs. SDL's Java half is
compiled straight out of `third_party/SDL/android-project`, so the submodule
moving forward moves both halves at once.

**CI should build the APK and stop.** The self-tests are host executables and
`gcgate.py`, `pclink.py` and `--drift` all stay on a desktop runner. A green
Android build proves it compiles and nothing more.

Two things about that job are worth knowing, both learned the hard way:

* `third_party/librw` points at a branch of THIS repository, which is private,
  so the submodule clone needs a token. `actions/checkout` puts its auth
  header in the superproject's local config and a submodule clone is a fresh
  git process that does not read it -- so the job asks for a username and
  dies. `url.insteadOf` in the global config is what reaches it.
* The upload step cannot fail the job. Artifact storage is an account-wide
  quota, and a build that compiled and packaged should not go red because
  there was nowhere to put a copy. What decides whether the APK is good is
  the step before it, which looks inside for `lib/arm64-v8a/libmain.so` --
  a misconfigured `externalNativeBuild` yields a valid APK with no native
  library in it and a green build.

### On the phone itself

You cannot build the APK on an Android device. The NDK ships for x86-64
Linux, macOS and Windows and for nothing else -- there is no aarch64-Linux
NDK -- so `externalNativeBuild` has no compiler there, and the Android Gradle
plugin fetches a `linux-x86_64` `aapt2` besides.

What you CAN do on the device, with no PC and no NDK, is **the arena probe**,
which is the one measurement that decides the ABI. Termux's clang targets
`aarch64-linux-android` natively, because Termux is an Android application:

```
pkg install clang git
clang -O2 -o arena_probe tools/android/arena_probe.c
./arena_probe
```

The caveat is the one in **The low 4 GB** above and no worse: a Termux binary
is exec'd from the Termux app rather than forked from the zygote with ART
mapped, so it is the same quality of evidence as the `adb push` route, on the
same kernel with the same `mmap_min_addr` and the same ASLR. It just needs no
computer.

---

## Phase order

1. **Measure.** Run the arena probe on a real device; pick the ABI from the
   result.
2. **Link and log.** NDK toolchain, `libmain.so`, Gradle shell, logcat.
   Success is reaching `iMemInit` and printing from a device.
3. **Draw.** ES profile fix, DXT fallback, verify each capture pass. Success
   is JF01 on screen.
4. **Play.** Asset import, save folder, controller (free), then touch
   (expensive).
5. **Survive.** Context loss, pause and resume, audio device change,
   rotation, back button.
6. **Polish.** Settings UI, phone defaults, high-refresh pacing.

1 and 2 are the bulk of the code. 4 is where the estimate is least
trustworthy, for the context-loss reason.

---

## What is still unknown

Listed rather than buried, because every one of them can invalidate work built
on top of it:

* **Whether an arm64 Android process can map the arena below 4 GB.** Phase 1.
  Unanswered until the probe runs on a device, and not settled even then until
  the game itself reaches `iMemInit`.
* ~~**Whether any of this compiles with a real NDK.**~~ **Answered: it does.**
  `.github/workflows/android.yml` builds `assembleDebug` for arm64-v8a on a
  runner with NDK r28c and produces an APK with `lib/arm64-v8a/libmain.so` in
  it -- the game, librw and SDL cross-compiled and linked. That is the phase 2
  milestone for the BUILD, and it is the whole of what it proves: nothing has
  started the application.
* **Whether the GLES profile fallback reaches ES 3.1 on a device**, and what
  the render targets and the snapshot readback do there.
* **What happens to the EGL context on backgrounding**, and which of the three
  answers above the port ends up needing.
* **Where the button glyphs live.** `src/SB/Core/pc/res/buttons` is staged
  beside the executable on a desktop; there is no executable directory on
  Android and the staging is skipped. They want the same treatment as the
  assets, or to be packed into the APK and extracted on first run.
