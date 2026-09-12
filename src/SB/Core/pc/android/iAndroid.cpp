#include "iAndroid.h"

#include <android/log.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include "iHost.h"

// See iAndroid.h for what this file is for and why it is in two halves.

namespace
{
    const char* const kTag = "bfbb";

    // ---------------------------------------------------------------------
    // stdout and stderr to logcat

    int sLogPipe[2] = { -1, -1 };

    // One log line per printf line, rather than per write.
    //
    // bfbb_main.cpp sets both streams unbuffered so that a crash cannot
    // discard the tail, which means a single printf can arrive here as
    // several reads and several printfs can arrive as one. logcat has no
    // notion of a partial line -- each call is a record with its own
    // timestamp -- so the pieces are reassembled on '\n' before being handed
    // over, or the log comes out as confetti.
    void* iAndroidLogPump(void*)
    {
        char line[1024];
        size_t used = 0;

        for (;;)
        {
            char chunk[256];
            ssize_t got = read(sLogPipe[0], chunk, sizeof(chunk));

            if (got < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                break;
            }

            if (got == 0)
            {
                break;
            }

            for (ssize_t i = 0; i < got; i++)
            {
                const char c = chunk[i];

                // A line longer than the buffer is split rather than dropped.
                // The port prints a few very long lines -- the asset search
                // path, a demangled template -- and half of one is worth more
                // than none of it.
                if (c == '\n' || used == sizeof(line) - 1)
                {
                    line[used] = '\0';
                    __android_log_write(ANDROID_LOG_INFO, kTag, line);
                    used = 0;

                    if (c == '\n')
                    {
                        continue;
                    }
                }

                // Carriage returns come from the progress lines, which print
                // '\r' to redraw in place. There is nothing to redraw in a
                // log, and a stray CR inside a record is rendered as a box.
                if (c != '\r')
                {
                    line[used++] = c;
                }
            }
        }

        // The pipe only closes when the process is going down.
        if (used != 0)
        {
            line[used] = '\0';
            __android_log_write(ANDROID_LOG_INFO, kTag, line);
        }

        return NULL;
    }

    // ---------------------------------------------------------------------
    // Directories

    // SDL hands back a pointer it owns, and NULL when the framework has not
    // made that directory available -- external storage on a device with the
    // volume unmounted, most often. NULL is a real answer and not a failure:
    // the host seam reports "the host cannot say" for whatever depends on it
    // and every caller of those has a fallback.
    void iAndroidSetDir(const char* name, const char* path)
    {
        if (path == NULL || path[0] == '\0')
        {
            return;
        }

        iHostSetEnv(name, path);
    }
}

void iAndroidOpenLog()
{
    if (sLogPipe[0] != -1)
    {
        return;
    }

    if (pipe(sLogPipe) != 0)
    {
        __android_log_write(ANDROID_LOG_ERROR, kTag,
                            "could not open a pipe for stdout; the log is lost");
        sLogPipe[0] = -1;
        return;
    }

    // dup2 onto the two standard descriptors rather than replacing the FILE*
    // streams: everything that prints does so through fd 1 or fd 2 in the
    // end, including SDL's own logging and anything librw prints, and a
    // freopen would only catch the C library's half of that.
    dup2(sLogPipe[1], STDOUT_FILENO);
    dup2(sLogPipe[1], STDERR_FILENO);

    pthread_t pump;
    if (pthread_create(&pump, NULL, iAndroidLogPump, NULL) != 0)
    {
        __android_log_write(ANDROID_LOG_ERROR, kTag,
                            "could not start the log pump; the log is lost");
        return;
    }

    // Never joined. It lives as long as the process and has nothing to
    // report when it ends, and a detached thread is one fewer handle for a
    // shutdown path to remember.
    pthread_detach(pump);
}

void iAndroidStartup()
{
    const char* internal = SDL_GetAndroidInternalStoragePath();
    const char* cache = SDL_GetAndroidCachePath();
    const char* external = SDL_GetAndroidExternalStoragePath();

    iAndroidSetDir("BFBB_ANDROID_INTERNAL", internal);
    iAndroidSetDir("BFBB_ANDROID_CACHE", cache);
    iAndroidSetDir("BFBB_ANDROID_EXTERNAL", external);

    // Where the assets are, unless something already said.
    //
    // getExternalFilesDir is the only place on a modern Android that both the
    // player and the game can reach: it needs no permission from this side
    // and it is visible over USB and to a file manager from the other, which
    // is how thirty gigabytes of retail data get onto the device at all.
    // fopen on a path the player picked somewhere else has not worked since
    // API 30.
    //
    // Only when the directory is actually there, and only when BFBB_ASSETS is
    // not already set. Absent both, `[assets] path` in config.ini decides, as
    // it does on every other host -- so a build that keeps its data somewhere
    // unusual is still configurable, and the documented precedence
    // (BFBB_ASSETS beats the settings file) is not quietly inverted here.
    if (external != NULL && external[0] != '\0')
    {
        const char* already = getenv("BFBB_ASSETS");

        if (already == NULL || already[0] == '\0')
        {
            char assets[512];
            snprintf(assets, sizeof(assets), "%s/assets", external);

            if (iHostPathExists(assets))
            {
                iHostSetEnv("BFBB_ASSETS", assets);
            }
        }
    }

    // Printed rather than merely set, because "the game says it has no
    // assets" is the first thing that will go wrong on a device and the
    // answer is always one of these three lines.
    printf("bfbb: android -- internal %s\n", internal != NULL ? internal : "(none)");
    printf("bfbb: android -- external %s\n", external != NULL ? external : "(none)");

    const char* assets = getenv("BFBB_ASSETS");
    printf("bfbb: android -- assets %s\n",
           assets != NULL && assets[0] != '\0' ? assets : "(from config.ini)");
    fflush(stdout);
}
