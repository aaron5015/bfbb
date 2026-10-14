#ifndef ICOLLIDEFAST_H
#define ICOLLIDEFAST_H

#if defined(XBOX)
struct xScene;
#else
#include "xScene.h"
#endif

void iCollideFastInit(xScene* sc);

#endif
