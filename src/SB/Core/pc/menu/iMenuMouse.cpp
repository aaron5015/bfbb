// The mouse in the game's menus. The argument for the shape of it is in
// iMenuMouse.h.

#include "iMenuMouse.h"

#include "iPadHost.h"
#include "iScreen.h"
#include "iWindow.h"
#include "xEnt.h"
#include "xEvent.h"
#include "xPad.h"
#include "zGlobals.h"
#include "zScene.h"
#include "zUI.h"
#include "zUIFont.h"

namespace
{
    // The pointer in the menus' own units, 640x480 across the UI box.
    F32 sX;
    F32 sY;
    S32 sShowing;
    S32 sMoved;
    S32 sClicked;

    // A screen's row was under the pointer last frame, so the glove shows.
    S32 sRowOver;
    S32 sRowOverNext;

    S32 sOwned;
    S32 sAcceptQueued;

    // Window pixels to the menus' units: through the letterbox every backend
    // puts the render size in, then through the centred 4:3 UI box.
    S32 ToMenu(F32 wx, F32 wy, F32* mx, F32* my)
    {
        S32 ww = 0;
        S32 wh = 0;
        iWindowGetSize(&ww, &wh);
        F32 rw = (F32)iScreenWidth();
        F32 rh = (F32)iScreenHeight();
        if (ww <= 0 || wh <= 0 || rw <= 0.0f || rh <= 0.0f)
        {
            return FALSE;
        }

        F32 s = (F32)ww / rw;
        if (rh * s > (F32)wh)
        {
            s = (F32)wh / rh;
        }
        F32 rx = (wx - 0.5f * ((F32)ww - rw * s)) / s;
        F32 ry = (wy - 0.5f * ((F32)wh - rh * s)) / s;

        *mx = (rx - iScreenUIOriginXF()) / iScreenUIWidthF() * 640.0f;
        *my = (ry - iScreenUIOriginYF()) / iScreenUIHeightF() * 480.0f;
        return TRUE;
    }

    bool IsUI(const xBase* b)
    {
        return b != NULL && (b->baseType == eBaseTypeUI || b->baseType == eBaseTypeUIFont);
    }

    // Under the pointer, and how far from the middle of the option, for the
    // menus whose options overlap -- the title list's are 60 high and 30
    // apart. -1 when not under it.
    F32 Distance(const _zUI* ui)
    {
        const zUIAsset* a = ui->sasset;
        F32 x0 = a->pos.x;
        F32 y0 = a->pos.y;
        F32 x1 = x0 + a->dim[0];
        F32 y1 = y0 + a->dim[1];
        if (sX < x0 || sX >= x1 || sY < y0 || sY >= y1)
        {
            return -1.0f;
        }
        F32 dx = sX - 0.5f * (x0 + x1);
        F32 dy = sY - 0.5f * (y0 + y1);
        return dx * dx + dy * dy;
    }

    bool HasLink(const xBase* b, U32 srcEvent)
    {
        for (S32 i = 0; i < b->linkCount; i++)
        {
            if (b->link[i].srcEvent == srcEvent)
            {
                return true;
            }
        }
        return false;
    }

    bool IsMove(U32 srcEvent)
    {
        return srcEvent == eEventPadPressUp || srcEvent == eEventPadPressDown ||
               srcEvent == eEventPadPressLeft || srcEvent == eEventPadPressRight;
    }

    // Live options: shown, enabled, and in the group that has focus. The
    // managers that sit selected under every menu are a few pixels across and
    // have nothing to point at.
    bool IsLive(const _zUI* ui)
    {
        return xEntIsVisible(ui) && (ui->baseFlags & 1) && (ui->uiFlags & 0x8) &&
               ui->sasset->dim[0] >= 8 && ui->sasset->dim[1] >= 8;
    }

    // Every pad press the selected options' links answer with a move, walked
    // outward from them. kMax is more options than any one menu shows.
    const S32 kMax = 256;

    struct Step
    {
        xBase* node;
        S32 from; // index of the step it was reached from, -1 at a start
        U32 event; // the press, sent to `from`'s node, that selects this one
    };

    Step sSteps[kMax];
    S32 sStepCount;

    S32 FindStep(const xBase* b)
    {
        for (S32 i = 0; i < sStepCount; i++)
        {
            if (sSteps[i].node == b)
            {
                return i;
            }
        }
        return -1;
    }

    void AddStart(_zUI* ui)
    {
        if ((ui->baseFlags & 1) && (ui->uiFlags & 0x8) && (ui->uiFlags & 0x2) &&
            sStepCount < kMax && FindStep(ui) < 0)
        {
            sSteps[sStepCount].node = ui;
            sSteps[sStepCount].from = -1;
            sSteps[sStepCount].event = 0;
            sStepCount++;
        }
    }

    void Walk(zScene* s)
    {
        sStepCount = 0;

        _zUI* ui = (_zUI*)s->baseList[eBaseTypeUI];
        for (U32 i = 0; i < s->baseCount[eBaseTypeUI]; i++)
        {
            AddStart(&ui[i]);
        }
        zUIFont* font = (zUIFont*)s->baseList[eBaseTypeUIFont];
        for (U32 i = 0; i < s->baseCount[eBaseTypeUIFont]; i++)
        {
            AddStart(&font[i]);
        }

        for (S32 at = 0; at < sStepCount; at++)
        {
            xBase* b = sSteps[at].node;
            for (S32 l = 0; l < b->linkCount; l++)
            {
                const xLinkAsset& link = b->link[l];
                if (!IsMove(link.srcEvent) ||
                    (link.dstEvent != eEventUISelect && link.dstEvent != eEventUIFocusOn_Select) ||
                    (link.chkAssetID != 0 && link.chkAssetID != b->id))
                {
                    continue;
                }

                xBase* to = zSceneFindObject(link.dstAssetID);
                if (!IsUI(to) || FindStep(to) >= 0 || sStepCount == kMax)
                {
                    continue;
                }

                sSteps[sStepCount].node = to;
                sSteps[sStepCount].from = at;
                sSteps[sStepCount].event = link.srcEvent;
                sStepCount++;
            }
        }
    }

    // Sends the presses that walk the selection to step `at`, first one first.
    void Follow(S32 at)
    {
        S32 path[kMax];
        S32 n = 0;
        while (at >= 0 && sSteps[at].from >= 0 && n < kMax)
        {
            path[n++] = at;
            at = sSteps[at].from;
        }

        for (S32 i = n - 1; i >= 0; i--)
        {
            xBase* from = sSteps[sSteps[path[i]].from].node;
            zEntEvent(from, from, sSteps[path[i]].event);
        }
    }

    // The option under the pointer that a pad could select, or NULL.
    _zUI* Pick(zScene* s, S32* step)
    {
        _zUI* best = NULL;
        F32 bestDistance = 0.0f;
        *step = -1;

        for (S32 type = 0; type < 2; type++)
        {
            U32 count = s->baseCount[type == 0 ? eBaseTypeUI : eBaseTypeUIFont];
            for (U32 i = 0; i < count; i++)
            {
                _zUI* ui = (type == 0) ? &((_zUI*)s->baseList[eBaseTypeUI])[i] :
                                         &((zUIFont*)s->baseList[eBaseTypeUIFont])[i];
                if (!IsLive(ui))
                {
                    continue;
                }

                F32 d = Distance(ui);
                if (d < 0.0f || (best != NULL && d >= bestDistance))
                {
                    continue;
                }

                // Reachable, or already selected and something to accept.
                S32 at = FindStep(ui);
                bool selectable =
                    at >= 0 && (sSteps[at].from >= 0 || HasLink(ui, eEventPadPressX) ||
                                HasLink(ui, eEventPadPressStart));
                if (!selectable)
                {
                    continue;
                }

                best = ui;
                bestDistance = d;
                *step = at;
            }
        }

        return best;
    }
} // namespace

U32 iMenuMouseUpdate(S32 inMenu)
{
    sRowOver = sRowOverNext;
    sRowOverNext = 0;
    sMoved = 0;
    sClicked = 0;

    S32 accept = sAcceptQueued;
    sAcceptQueued = 0;

    // A scene still loading has counts before it has lists.
    zScene* s = globals.sceneCur;
    if (!inMenu || s == NULL ||
        (s->baseCount[eBaseTypeUI] != 0 && s->baseList[eBaseTypeUI] == NULL) ||
        (s->baseCount[eBaseTypeUIFont] != 0 && s->baseList[eBaseTypeUIFont] == NULL))
    {
        sShowing = iPadHostSetPointer(FALSE, FALSE);
        return 0;
    }

    F32 wx = 0.0f;
    F32 wy = 0.0f;
    S32 moved = 0;
    S32 clicked = 0;
    S32 wheel = 0;
    sShowing = iPadHostPointer(&wx, &wy, &moved, &clicked, &wheel) && ToMenu(wx, wy, &sX, &sY);

    _zUI* over = NULL;
    S32 step = -1;
    if (sShowing && !sOwned)
    {
        Walk(s);
        over = Pick(s, &step);
    }

    sShowing = iPadHostSetPointer(TRUE, over != NULL || sRowOver);
    if (!sShowing)
    {
        return 0;
    }

    sMoved = moved;
    sClicked = clicked;

    U32 add = accept ? XPAD_BUTTON_X : 0;

    // The wheel steps through a list the way the d-pad does.
    if (wheel > 0)
    {
        add |= XPAD_BUTTON_UP;
    }
    else if (wheel < 0)
    {
        add |= XPAD_BUTTON_DOWN;
    }

    if (over != NULL && (moved || clicked) && sSteps[step].from >= 0)
    {
        Follow(step);
    }

    if (over != NULL && clicked)
    {
        bool startOnly = !HasLink(over, eEventPadPressX) && HasLink(over, eEventPadPressStart);
        add |= startOnly ? XPAD_BUTTON_START : XPAD_BUTTON_X;
    }

    return add;
}

S32 iMenuMouseOver(xBase* ent)
{
    if (!sShowing || !IsUI(ent))
    {
        return FALSE;
    }

    _zUI* ui = (_zUI*)ent;
    if (!xEntIsVisible(ui) || Distance(ui) < 0.0f)
    {
        return FALSE;
    }

    sRowOverNext = 1;
    return TRUE;
}

S32 iMenuMouseMoved()
{
    return sShowing && sMoved;
}

S32 iMenuMouseClicked()
{
    return sShowing && sClicked;
}

void iMenuMouseOwn(S32 on)
{
    sOwned = on ? 1 : 0;
}

void iMenuMouseAccept()
{
    sAcceptQueued = 1;
}
