// RenderWare C API: RpHAnim, the hierarchical animation plugin.
//
// This is the skeleton behind every animated model in the game: a tree of
// nodes, one matrix per node, and an atomic's skin weighted against them.
// iModel.cpp builds one per model as it loads (iModelFileNew), and xAnim drives
// it every frame.
//
// RpHAnimHierarchy is mirrored onto rw::HAnimHierarchy in include/rwsdk/rphanim.h
// with librw's field order under RenderWare's names, minus rootParentOffset,
// which librw does not have and nothing reads. RpHAnimNodeInfo needed no
// reordering at all -- {nodeID, nodeIndex, flags, pFrame} is librw's
// {id, index, flags, frame} field for field.
//
// The plugin itself is attached in plugins.cpp, by RpHAnimPluginAttach, which
// until now was the only thing the port did with HAnim.

#include <rwcore.h>
#include <rphanim.h>
#include <rpskin.h>
#include <rpworld.h>

#include "rw.h"

static inline rw::HAnimHierarchy* asHierarchy(RpHAnimHierarchy* hierarchy)
{
    return reinterpret_cast<rw::HAnimHierarchy*>(hierarchy);
}

RpHAnimHierarchy* RpHAnimHierarchyCreate(RwInt32 numNodes, RwUInt32* nodeFlags, RwInt32* nodeIDs,
                                         RpHAnimHierarchyFlag flags, RwInt32 maxKeyFrameSize)
{
    if (numNodes <= 0)
    {
        return NULL;
    }

    // nodeFlags is RwUInt32* here and int32* in librw. Same width, same values
    // -- the flags are a small bit set (rpHANIMPUSHPARENTMATRIX and friends) and
    // nothing in the range is sign-sensitive.
    return reinterpret_cast<RpHAnimHierarchy*>(
        rw::HAnimHierarchy::create(numNodes, (rw::int32*)nodeFlags, (rw::int32*)nodeIDs,
                                   (rw::int32)flags, maxKeyFrameSize));
}

RwBool RpHAnimFrameSetHierarchy(RwFrame* frame, RpHAnimHierarchy* hierarchy)
{
    if (frame == NULL)
    {
        return FALSE;
    }

    // librw keeps this in the frame's HAnimData plugin block, which is what
    // RpHAnimPluginAttach registered. Setting it on a frame whose plugin was
    // never attached would write past the frame; the attach is checked here
    // rather than assumed because the failure is silent memory corruption.
    rw::HAnimData* data = rw::HAnimData::get(reinterpret_cast<rw::Frame*>(frame));
    if (data == NULL)
    {
        return FALSE;
    }

    data->hierarchy = asHierarchy(hierarchy);

    // The frame owns the hierarchy: librw's frame destructor frees it only when
    // parentFrame is that frame. iModel.cpp creates one for every skinned model
    // whose stream carries none, and frees it by destroying the frame.
    if (hierarchy != NULL)
    {
        data->hierarchy->parentFrame = reinterpret_cast<rw::Frame*>(frame);
    }
    return TRUE;
}

RpHAnimHierarchy* RpHAnimFrameGetHierarchy(RwFrame* frame)
{
    if (frame == NULL)
    {
        return NULL;
    }

    rw::HAnimData* data = rw::HAnimData::get(reinterpret_cast<rw::Frame*>(frame));
    if (data == NULL)
    {
        return NULL;
    }

    return reinterpret_cast<RpHAnimHierarchy*>(data->hierarchy);
}
