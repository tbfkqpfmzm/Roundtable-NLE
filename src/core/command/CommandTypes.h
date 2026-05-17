#pragma once

#include <cstdint>

namespace rt {

// ── Command type IDs for merge support ──────────────────────────────────────
// Centralized enum to eliminate circular header dependencies.
// Each command header includes this file instead of ClipCommands.h.

enum class CommandTypeId : int
{
    AddClip           = 100,
    RemoveClip        = 101,
    MoveClip          = 102,
    TrimClipLeft      = 103,
    TrimClipRight     = 104,
    SetClipSpeed      = 105,
    SetClipEnabled    = 106,
    SetClipLabel      = 107,
    SetClipColor      = 108,
    AddTrack          = 200,
    RemoveTrack       = 201,
    MoveTrack         = 202,
    SetTrackName      = 203,
    SetTrackLocked    = 204,
    SetTrackMuted     = 205,
    SetTrackSoloed    = 206,
    SetTrackHeight    = 207,
    AddKeyframe       = 300,
    RemoveKeyframe    = 301,
    MoveKeyframe      = 302,
    SetKeyframeInterp = 303,
    AddTransition     = 400,
    RemoveTransition  = 401,
};

enum class EffectCmdId : int
{
    AddEffect         = 500,
    RemoveEffect      = 501,
    MoveEffect        = 502,
    SetEffectParam    = 503,
    SetEffectEnabled  = 504,
};

enum class MarkerCommandTypeId : int
{
    AddMarker    = 600,
    RemoveMarker = 601,
};

} // namespace rt