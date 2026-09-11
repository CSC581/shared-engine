#pragma once

#include <cstdint>

// What one frame knows about time.
//
// The engine builds this once per frame and hands it down to the game and its
// entities, so nothing below the engine owns a clock or asks one what time it
// is. Both fields are already on the game timeline: they stop advancing while
// it is paused and run slower or faster with its scale.
struct FrameTime {
    // Game seconds since the previous frame, already clamped. Floating point
    // appears here, at the boundary, and nowhere inside the clocks themselves.
    double dtSeconds;

    // Absolute game time in game microseconds. Motion computed as a function
    // of this (a platform on a sine path, say) never drifts, however uneven
    // the frames that sampled it were.
    std::int64_t gameTimeUs;
};
