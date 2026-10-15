// The pause menu's task spatulas, kept spinning in step. What drifts and why is
// in iMenuSpin.h.

#include "iMenuSpin.h"

#include "iFixes.h"

#include "xAnim.h"
#include "xEnt.h"
#include "xModel.h"

namespace
{
    // The model's one running animation, or NULL if it has none to share.
    xAnimSingle* iSpinSingle(const xEnt* ent)
    {
        if (ent == NULL || ent->model == NULL || ent->model->Anim == NULL)
        {
            return NULL;
        }

        xAnimSingle* single = ent->model->Anim->Single;
        if (single == NULL || single->State == NULL || single->State->Data == NULL)
        {
            return NULL;
        }

        return single;
    }
} // namespace

void iMenuSpinSync(xEnt* const* ents, S32 count)
{
    if (!iFixSpatulaSpin())
    {
        return;
    }

    // Every spatula was given the same time last frame, and the visible ones
    // have all been advanced by the same step since, so any visible one is the
    // clock.
    const xAnimSingle* lead = NULL;
    for (S32 i = 0; i < count && lead == NULL; i++)
    {
        if (xEntIsVisible(ents[i]))
        {
            lead = iSpinSingle(ents[i]);
        }
    }

    if (lead == NULL)
    {
        return;
    }

    for (S32 i = 0; i < count; i++)
    {
        xAnimSingle* single = iSpinSingle(ents[i]);
        if (single == NULL || single == lead ||
            single->State->Data->Duration != lead->State->Data->Duration)
        {
            continue;
        }

        if (single->Time == lead->Time)
        {
            continue;
        }

        single->Time = lead->Time;
        single->LastTime = lead->LastTime;

        if (xEntIsVisible(ents[i]))
        {
            xModelEval(ents[i]->model);
        }
    }
}
