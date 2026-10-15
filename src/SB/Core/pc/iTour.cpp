#include "iTour.h"

#include "iFrameGrab.h"
#include "iHost.h"
#include "iPadBind.h"
#include "iPadHost.h"
#include "iSnapshot.h"
#include "iSndHost.h"
#include "iTime.h"

#include "zGlobals.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
    enum Op
    {
        OP_WAIT,
        OP_HOLD,
        OP_MOVE,
        OP_GOTO,
        OP_SHOT,
        OP_RECORD,
        OP_STOP,
        OP_POS,
        OP_SEED,
        OP_QUIT
    };

    // The seed a tour pins until a `seed` step says otherwise.
    const U32 kDefaultSeed = 1;

    struct Step
    {
        Op op;
        S32 frames;
        U32 buttons;
        U32 taps; // move, goto: pressed kTapDown of every kTapPeriod frames
        S32 width;
        F32 sticks[4]; // left x, y, right x, y
        F32 target[3]; // goto: x, z, tolerance
        U32 seed;
        char name[64];
    };

    const S32 kMaxSteps = 16384;
    const S32 kRelease = 4;
    const S32 kTapPeriod = 12;
    const S32 kTapDown = 4;
    const S32 kShotWidth = 1280;

    // A shot taken while a loading screen holds the snapshot has no frame to
    // read; it is retried for this many frames before giving up.
    const S32 kShotRetry = 600;

    Step sSteps[kMaxSteps];
    S32 sCount;
    S32 sAt;
    S32 sLeft;
    S32 sReleasing;
    S32 sElapsed;
    S32 sFrameNo;
    char sOutDir[512];

    void Fail(S32 line, const char* what, const char* text)
    {
        printf("bfbb: tour: line %d: %s: %s\n", (int)line, what, text);
        fflush(stdout);
        exit(1);
    }

    // Space-separated button names from `p` into a mask. Empty is allowed
    // where `optional` is set. With `taps`, a name written *NAME goes there
    // instead.
    U32 Buttons(char* p, S32 line, bool optional, U32* taps = NULL)
    {
        U32 mask = 0;
        for (char* tok = strtok(p, " \t"); tok != NULL; tok = strtok(NULL, " \t"))
        {
            bool tap = taps != NULL && tok[0] == '*';
            const iPadBindButton* b = iPadBindFind(tap ? tok + 1 : tok);
            if (b == NULL)
            {
                Fail(line, "unknown button", tok);
            }
            if (tap)
            {
                *taps |= b->mask;
            }
            else
            {
                mask |= b->mask;
            }
        }
        if (mask == 0 && (taps == NULL || *taps == 0) && !optional)
        {
            Fail(line, "no buttons", "");
        }
        return mask;
    }

    // ---------------------------------------------------------------------
    // Recording: every presented frame piped to ffmpeg as raw RGB, the sound
    // mixed on the tour's clock into a WAV beside it, and the two muxed into
    // NAME.mp4 when the recording stops. The clock moves 1/60 s per frame, so
    // a 60 fps video plays at the game's own speed whatever the machine did.

    struct Recording
    {
        bool on;
        char base[640];
        S32 width;
        FILE* video;
        S32 w;
        S32 h;
        U8* last;
        S32 frames;
        FILE* wav;
        U32 rate;
        U32 samples;
    };

    Recording sRec;

    const char* Ffmpeg()
    {
        const char* f = getenv("BFBB_FFMPEG");
        return f != NULL && f[0] != '\0' ? f : "ffmpeg";
    }

    void PutLE(U8* p, U32 v, S32 bytes)
    {
        for (S32 i = 0; i < bytes; i++)
        {
            p[i] = (U8)(v >> (8 * i));
        }
    }

    void WavHeader(FILE* f, U32 rate, U32 samples)
    {
        U8 h[44];
        U32 data = samples * 4;
        memcpy(h, "RIFF", 4);
        PutLE(h + 4, 36 + data, 4);
        memcpy(h + 8, "WAVEfmt ", 8);
        PutLE(h + 16, 16, 4);
        PutLE(h + 20, 1, 2); // PCM
        PutLE(h + 22, 2, 2); // stereo
        PutLE(h + 24, rate, 4);
        PutLE(h + 28, rate * 4, 4);
        PutLE(h + 32, 4, 2);
        PutLE(h + 34, 16, 2);
        memcpy(h + 36, "data", 4);
        PutLE(h + 40, data, 4);
        fwrite(h, 1, sizeof(h), f);
    }

    void SoundSink(const float* stereo, U32 frames, U32 rate)
    {
        if (!sRec.on)
        {
            return;
        }
        if (sRec.wav == NULL)
        {
            char path[700];
            snprintf(path, sizeof(path), "%s.wav", sRec.base);
            sRec.wav = fopen(path, "wb");
            if (sRec.wav == NULL)
            {
                return;
            }
            sRec.rate = rate;
            WavHeader(sRec.wav, rate, 0);
        }

        S16 buf[2048];
        U32 done = 0;
        while (done < frames * 2)
        {
            U32 n = frames * 2 - done;
            if (n > 2048)
            {
                n = 2048;
            }
            for (U32 i = 0; i < n; i++)
            {
                float s = stereo[done + i] * 32767.0f;
                buf[i] = (S16)(s > 32767.0f ? 32767 : s < -32768.0f ? -32768 : (S32)s);
            }
            fwrite(buf, sizeof(S16), n, sRec.wav);
            done += n;
        }
        sRec.samples += frames;
    }

    void RecordStart(const Step* s)
    {
        memset(&sRec, 0, sizeof(sRec));
        snprintf(sRec.base, sizeof(sRec.base), "%s%s", sOutDir, s->name);
        sRec.width = s->width;
        sRec.on = true;
        iSndHostSetSink(SoundSink);
        printf("bfbb: tour: recording %s.mp4 from frame %d\n", sRec.base, (int)sFrameNo);
        fflush(stdout);
    }

    // One presented frame. A frame that cannot be read (a loading screen
    // holding the snapshot) repeats the last one, so the video keeps time with
    // the sound.
    void RecordFrame()
    {
        S32 w = 0, h = 0;
        U8* rgb = iFrameGrabRGB(sRec.width, &w, &h);

        if (rgb != NULL && sRec.video == NULL)
        {
            char cmd[2048];
            snprintf(cmd, sizeof(cmd),
                     "\"%s\" -y -loglevel error -f rawvideo -pix_fmt rgb24 -s %dx%d -framerate 60 "
                     "-i - -vf pad=ceil(iw/2)*2:ceil(ih/2)*2 -c:v libx264 -preset veryfast "
                     "-crf 18 -pix_fmt yuv420p \"%s.video.mp4\"",
                     Ffmpeg(), (int)w, (int)h, sRec.base);
            sRec.video = iHostPipeOpen(cmd);
            if (sRec.video == NULL)
            {
                printf("bfbb: tour: cannot start %s; not recording\n", Ffmpeg());
                fflush(stdout);
                sRec.on = false;
                iSndHostSetSink(NULL);
                free(rgb);
                return;
            }
            sRec.w = w;
            sRec.h = h;
        }

        if (rgb != NULL && (w != sRec.w || h != sRec.h))
        {
            free(rgb);
            rgb = NULL;
        }
        if (rgb != NULL)
        {
            free(sRec.last);
            sRec.last = rgb;
        }
        if (sRec.video != NULL && sRec.last != NULL)
        {
            fwrite(sRec.last, 1, (size_t)sRec.w * sRec.h * 3, sRec.video);
            sRec.frames++;
        }
    }

    void RecordStop()
    {
        if (!sRec.on)
        {
            return;
        }
        sRec.on = false;
        iSndHostSetSink(NULL);

        char video[700], wav[700], out[700];
        snprintf(video, sizeof(video), "%s.video.mp4", sRec.base);
        snprintf(wav, sizeof(wav), "%s.wav", sRec.base);
        snprintf(out, sizeof(out), "%s.mp4", sRec.base);

        if (sRec.video != NULL)
        {
            iHostPipeClose(sRec.video);
        }
        free(sRec.last);

        if (sRec.wav != NULL)
        {
            fseek(sRec.wav, 0, SEEK_SET);
            WavHeader(sRec.wav, sRec.rate, sRec.samples);
            fclose(sRec.wav);
        }

        if (sRec.video == NULL)
        {
            printf("bfbb: tour: recording %s has no frames\n", sRec.base);
        }
        else if (sRec.wav != NULL && sRec.samples > 0)
        {
            char cmd[2400];
            snprintf(cmd, sizeof(cmd),
                     "\"%s\" -y -loglevel error -i \"%s\" -i \"%s\" -c:v copy -c:a aac -b:a 192k "
                     "-shortest \"%s\"",
                     Ffmpeg(), video, wav, out);
            FILE* p = iHostPipeOpen(cmd);
            if (p != NULL && iHostPipeClose(p) == 0)
            {
                iHostRemoveFile(video);
                iHostRemoveFile(wav);
            }
            printf("bfbb: tour: wrote %s (%d frames, %.1f s of sound)\n", out, (int)sRec.frames,
                   (double)sRec.samples / (sRec.rate ? sRec.rate : 1));
        }
        else
        {
            iHostRenameReplace(video, out);
            printf("bfbb: tour: wrote %s (%d frames, no sound)\n", out, (int)sRec.frames);
        }
        fflush(stdout);
    }

    void Finish(const char* why)
    {
        RecordStop();
        printf("bfbb: tour: %s\n", why);
        fflush(stdout);
        exit(0);
    }

    // ---------------------------------------------------------------------

    void Pos()
    {
        xEnt& p = globals.player.ent;
        if (p.model == NULL || p.model->Mat == NULL)
        {
            printf("bfbb: tour: frame %d no player yet\n", (int)sFrameNo);
            fflush(stdout);
            return;
        }
        const xVec3* at = p.frame != NULL ? &p.frame->mat.at : NULL;
        const xVec3& pos = *xEntGetPos(&p);
        const xVec3& cam = globals.camera.mat.at;
        F32 yaw = at != NULL ? atan2f(at->x, at->z) * 57.29578f : 0.0f;
        F32 camyaw = atan2f(cam.x, cam.z) * 57.29578f;
        printf("bfbb: tour: frame %d pos %.2f %.2f %.2f yaw %.1f camyaw %.1f\n", (int)sFrameNo,
               pos.x, pos.y, pos.z, yaw, camyaw);
        fflush(stdout);
    }

    // goto: point the left stick at a world xz position, in the camera's frame,
    // since the game reads the stick relative to the camera. TRUE once within
    // the tolerance. The stick eases off over the last three units so the
    // player stops on the point instead of circling it.
    bool Steer(const Step* s)
    {
        xEnt& p = globals.player.ent;
        if (p.model == NULL || p.model->Mat == NULL)
        {
            iPadHostScriptSticks(0.0f, 0.0f, 0.0f, 0.0f);
            return false;
        }

        const xVec3& pos = *xEntGetPos(&p);
        F32 dx = s->target[0] - pos.x;
        F32 dz = s->target[1] - pos.z;
        F32 dist = sqrtf(dx * dx + dz * dz);
        if (dist <= s->target[2])
        {
            iPadHostScriptSticks(0.0f, 0.0f, 0.0f, 0.0f);
            return true;
        }

        const xVec3& at = globals.camera.mat.at;
        const xVec3& right = globals.camera.mat.right;
        F32 fl = sqrtf(at.x * at.x + at.z * at.z);
        F32 rl = sqrtf(right.x * right.x + right.z * right.z);
        if (fl < 0.001f || rl < 0.001f)
        {
            return false;
        }

        F32 ux = dx / dist;
        F32 uz = dz / dist;
        // The camera's right vector points to the left of the screen.
        F32 forward = (ux * at.x + uz * at.z) / fl;
        F32 side = -(ux * right.x + uz * right.z) / rl;
        F32 mag = dist < 3.0f ? 0.35f + 0.65f * dist / 3.0f : 1.0f;
        iPadHostScriptSticks(side * mag, forward * mag, 0.0f, 0.0f);
        return false;
    }

    void Load(const char* path)
    {
        FILE* f = fopen(path, "r");
        if (f == NULL)
        {
            printf("bfbb: tour: cannot open %s\n", path);
            fflush(stdout);
            exit(1);
        }

        // Whether a step that takes frames has been read yet.
        bool timed = false;

        char text[512];
        S32 line = 0;
        while (fgets(text, sizeof(text), f) != NULL)
        {
            line++;

            char* hash = strchr(text, '#');
            if (hash != NULL)
            {
                *hash = '\0';
            }
            text[strcspn(text, "\r\n")] = '\0';

            char* p = text;
            while (*p == ' ' || *p == '\t')
            {
                p++;
            }
            if (*p == '\0')
            {
                continue;
            }

            if (sCount == kMaxSteps)
            {
                Fail(line, "too many steps", "");
            }

            char* cmd = p;
            p += strcspn(p, " \t");
            if (*p != '\0')
            {
                *p++ = '\0';
            }

            Step* s = &sSteps[sCount];
            memset(s, 0, sizeof(*s));

            if (strcmp(cmd, "wait") == 0)
            {
                s->op = OP_WAIT;
                s->frames = atoi(p);
            }
            else if (strcmp(cmd, "press") == 0)
            {
                s->op = OP_HOLD;
                s->frames = 4;
                s->buttons = Buttons(p, line, false);
            }
            else if (strcmp(cmd, "hold") == 0)
            {
                s->op = OP_HOLD;
                s->frames = (S32)strtol(p, &p, 10);
                s->buttons = Buttons(p, line, false);
            }
            else if (strcmp(cmd, "move") == 0 || strcmp(cmd, "cam") == 0)
            {
                s->op = OP_MOVE;
                s->frames = (S32)strtol(p, &p, 10);
                F32 deg = (F32)strtod(p, &p);
                F32 mag = 1.0f;
                if (*p == '@')
                {
                    mag = (F32)strtod(p + 1, &p);
                }
                F32 x = sinf(deg / 57.29578f) * mag;
                F32 y = cosf(deg / 57.29578f) * mag;
                if (cmd[0] == 'm')
                {
                    s->sticks[0] = x;
                    s->sticks[1] = y;
                }
                else
                {
                    s->sticks[2] = x;
                    s->sticks[3] = y;
                }
                s->buttons = Buttons(p, line, true, &s->taps);
                if (s->frames <= 0)
                {
                    Fail(line, "move needs a frame count", cmd);
                }
            }
            else if (strcmp(cmd, "goto") == 0)
            {
                // goto X Z [TOL] [MAXFRAMES] [B...]
                s->op = OP_GOTO;
                s->target[0] = (F32)strtod(p, &p);
                s->target[1] = (F32)strtod(p, &p);
                s->target[2] = 1.5f;
                s->frames = 1800;
                char* q = p;
                F32 tol = (F32)strtod(p, &q);
                if (q != p)
                {
                    s->target[2] = tol;
                    p = q;
                    S32 max = (S32)strtol(p, &q, 10);
                    if (q != p)
                    {
                        s->frames = max;
                        p = q;
                    }
                }
                s->buttons = Buttons(p, line, true, &s->taps);
            }
            else if (strcmp(cmd, "shot") == 0 || strcmp(cmd, "record") == 0)
            {
                s->op = cmd[0] == 's' ? OP_SHOT : OP_RECORD;
                s->frames = kShotRetry;
                s->width = s->op == OP_SHOT ? kShotWidth : 0;
                char* name = strtok(p, " \t");
                if (name == NULL || strlen(name) >= sizeof(s->name))
                {
                    Fail(line, "needs a short name", p);
                }
                strcpy(s->name, name);
                char* width = strtok(NULL, " \t");
                if (width != NULL)
                {
                    s->width = atoi(width);
                }
            }
            else if (strcmp(cmd, "stop") == 0)
            {
                s->op = OP_STOP;
            }
            else if (strcmp(cmd, "pos") == 0)
            {
                s->op = OP_POS;
            }
            else if (strcmp(cmd, "seed") == 0)
            {
                s->op = OP_SEED;
                s->seed = (U32)strtoul(p, NULL, 0);
                // Before anything that takes a frame, it is the seed from the
                // start: a level booted into is seeded before the script's
                // first frame runs.
                if (!timed)
                {
                    iTimeSetSeed(s->seed);
                }
            }
            else if (strcmp(cmd, "quit") == 0)
            {
                s->op = OP_QUIT;
            }
            else
            {
                Fail(line, "unknown command", cmd);
            }

            if (s->op != OP_POS && s->op != OP_SEED && s->op != OP_RECORD && s->op != OP_STOP)
            {
                timed = true;
            }
            sCount++;
        }

        fclose(f);
    }

    void Begin()
    {
        if (sAt >= sCount)
        {
            Finish("done");
        }

        const Step* s = &sSteps[sAt];
        sLeft = s->frames;
        sReleasing = 0;
        sElapsed = 0;
        iPadHostScript(TRUE,
                       s->op == OP_HOLD || s->op == OP_MOVE || s->op == OP_GOTO ? s->buttons : 0);
        iPadHostScriptSticks(s->sticks[0], s->sticks[1], s->sticks[2], s->sticks[3]);
    }

    // The held buttons plus the tapped ones, down for the first kTapDown
    // frames of each kTapPeriod.
    void Tap(const Step* s)
    {
        if (s->taps != 0)
        {
            bool down = (++sElapsed % kTapPeriod) < kTapDown;
            iPadHostScript(TRUE, s->buttons | (down ? s->taps : 0));
        }
    }

    void Next()
    {
        sAt++;
        Begin();
    }

    void Frame()
    {
        iTimeStep();

        if (sRec.on)
        {
            RecordFrame();
        }
        sFrameNo++;

        // Steps that take no time run back to back in one frame.
        for (;;)
        {
            const Step* s = &sSteps[sAt];

            switch (s->op)
            {
            case OP_WAIT:
                if (--sLeft <= 0)
                {
                    Next();
                }
                return;

            case OP_HOLD:
                if (--sLeft > 0)
                {
                    return;
                }
                if (!sReleasing)
                {
                    sReleasing = 1;
                    sLeft = kRelease;
                    iPadHostScript(TRUE, 0);
                    return;
                }
                Next();
                return;

            case OP_MOVE:
                // No release: the next step takes over the sticks directly,
                // so moves chain into one continuous run.
                Tap(s);
                if (--sLeft <= 0)
                {
                    Next();
                }
                return;

            case OP_GOTO:
                Tap(s);
                if (Steer(s))
                {
                    printf("bfbb: tour: frame %d reached %.1f %.1f\n", (int)sFrameNo, s->target[0],
                           s->target[1]);
                    fflush(stdout);
                    Next();
                    continue;
                }
                if (--sLeft <= 0)
                {
                    printf("bfbb: tour: frame %d gave up on %.1f %.1f\n", (int)sFrameNo,
                           s->target[0], s->target[1]);
                    Pos();
                    Next();
                }
                return;

            case OP_SHOT:
            {
                char path[640];
                snprintf(path, sizeof(path), "%s%s.png", sOutDir, s->name);
                if (iFrameGrabWrite(path, s->width))
                {
                    printf("bfbb: tour: wrote %s\n", path);
                    fflush(stdout);
                    Next();
                }
                else if (--sLeft <= 0)
                {
                    printf("bfbb: tour: no frame for %s\n", s->name);
                    fflush(stdout);
                    Next();
                }
                return;
            }

            case OP_RECORD:
                RecordStop();
                RecordStart(s);
                Next();
                continue;

            case OP_STOP:
                RecordStop();
                Next();
                continue;

            case OP_POS:
                Pos();
                Next();
                continue;

            case OP_SEED:
                iTimeSetSeed(s->seed);
                Next();
                continue;

            case OP_QUIT:
                Finish("quit");
            }
        }
    }
} // namespace

S32 iTourActive()
{
    const char* path = getenv("BFBB_TOUR");
    return path != NULL && path[0] != '\0';
}

void iTourInit()
{
    if (!iTourActive())
    {
        return;
    }

    const char* script = getenv("BFBB_TOUR");
    iTimeSetSeed(kDefaultSeed);
    Load(script);

    const char* out = getenv("BFBB_TOUR_OUT");
    if (out != NULL && out[0] != '\0')
    {
        snprintf(sOutDir, sizeof(sOutDir), "%s/", out);
    }
    else
    {
        // The script's folder, with its separator.
        const char* slash = strrchr(script, '/');
        const char* back = strrchr(script, '\\');
        if (back != NULL && (slash == NULL || back > slash))
        {
            slash = back;
        }
        size_t n = slash != NULL ? (size_t)(slash - script + 1) : 0;
        if (n >= sizeof(sOutDir))
        {
            n = 0;
        }
        memcpy(sOutDir, script, n);
        sOutDir[n] = '\0';
    }

    printf("bfbb: tour: %d steps from %s, shots to %s\n", (int)sCount, script,
           sOutDir[0] != '\0' ? sOutDir : "the current folder");
    fflush(stdout);

    // iSystemInit already runs the fixed clock: it starts it before the sound.

    sAt = 0;
    Begin();
    iSnapshotSetFrameHook(Frame);
}
