#ifndef IFIRSTPERSON_H
#define IFIRSTPERSON_H

// The PC's first-person camera. The console has none, so both are the constant
// that leaves retail's code alone -- macros, because an inline function still
// changes CodeWarrior's code for the caller.
#define iFirstPersonCamera(cam, dt, d, h) 0
#define iFirstPersonHidesPlayer() 0

#endif
