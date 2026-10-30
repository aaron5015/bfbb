#ifndef ICOLOR_H
#define ICOLOR_H

#include <types.h>
#include <xHud.h>

// The assignment below is defined in the class, so xFont.h must not define it
// again; the GameCube's comes from xFont.h.
#define ICOLOR_ASSIGN_IN_CLASS

struct iColor_tag
{
    U8 r;
    U8 g;
    U8 b;
    U8 a;

    iColor_tag& operator=(const iColor_tag& rhs)
    {
        this->r = rhs.r;
        this->g = rhs.g;
        this->b = rhs.b;
        this->a = rhs.a;
        return *this;
    };

    // Conversion constructor instead?
    iColor_tag& operator=(const xhud::color32u& rhs)
    {
        this->r = rhs.r;
        this->g = rhs.g;
        this->b = rhs.b;
        this->a = rhs.a;
        return *this;
    };
};

#endif
