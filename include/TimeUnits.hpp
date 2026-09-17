#pragma once

#include <cstdint>

// Tic sizes shared by the clocks in the engine.
//
// Core time values are integer tics, never floating point: a tic count can be
// added, subtracted and compared forever without accumulating rounding error,
// which is what lets a timeline be paused, rescaled and re-anchored without
// drifting. What one tic means is decided by the clock that produced it.

constexpr std::int64_t kNsPerUs = 1'000;
constexpr std::int64_t kNsPerMs = 1'000'000;
constexpr std::int64_t kNsPerSec = 1'000'000'000;

// The game timeline counts microseconds. Fine enough that a frame is thousands
// of tics even at 0.25x, coarse enough that an int64_t holds ~292,000 years.
constexpr std::int64_t kGameTicsPerSecond = 1'000'000;
