#pragma once

#include "TimeSource.hpp"

#include <cstdint>

// "How much time has passed since I last looked", for one consumer.
//
// This is the per-consumer half of the time module: a Timeline says what time
// it is and nothing else, so every subsystem that needs a delta keeps its own
// DeltaTimer on it. Two consumers reading the same timeline at different rates
// therefore cannot steal each other's elapsed time.
//
// Lifetime: the source must outlive the timer.
//
// Thread safety: none, deliberately. A DeltaTimer belongs to exactly one
// consumer and is never shared across threads, so it pays for no lock.
class DeltaTimer {
public:
    // Starts from the source's current time, so the first tick() measures from
    // construction rather than from the source's epoch. Throws
    // std::invalid_argument if maxDelta is not positive.
    DeltaTimer(const TimeSource& source, std::int64_t maxDelta);

    // Tics since the previous tick(), clamped to [0, maxDelta]. Time dropped
    // by the clamp is gone, not carried over: a frame that stalled for two
    // seconds should be absorbed, not paid back over the frames after it.
    std::int64_t tick();

    // Forget the elapsed time without reporting it -- after a level load or a
    // long blocking operation, so the next tick() does not read as a stall.
    void reset();

    void setMaxDelta(std::int64_t maxDelta);
    std::int64_t maxDelta() const;

    // Whether the most recent tick() hit the clamp.
    bool lastWasClamped() const;

private:
    const TimeSource& source_;

    std::int64_t last_;
    std::int64_t maxDelta_;

    bool lastWasClamped_ = false;
};
