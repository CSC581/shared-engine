#pragma once

#include <atomic>
#include <cstdint>

// A monotonic source of integer tics.
//
// What one tic means is the source's own business: RealTimeClock counts
// nanoseconds, a Timeline counts whatever its tic size says, and ManualClock
// counts whatever a test advanced it by. Anything that reads time takes a
// TimeSource reference, so the same code runs against a real clock in the game
// and a hand-driven clock in a test.
//
// The whole time module is deliberately free of SDL and of global state, so a
// headless server can link it without a window system.
class TimeSource {
public:
    virtual ~TimeSource() = default;

    // Current time in this source's own tics. Never decreases.
    virtual std::int64_t now() const = 0;
};

// Real time, straight off the system's monotonic clock, in nanoseconds.
class RealTimeClock final : public TimeSource {
public:
    std::int64_t now() const override;
};

// A clock that counts loop iterations instead of elapsed time. The engine
// advances one of these once per pass through the main loop.
//
// This is the third scale the engine measures time on, beside real time and
// game time, and the only one that does not care how long anything took: a
// Timeline anchored here produces time in frames. That is what a simulation
// needs when every machine must step identically however fast each one runs --
// lockstep peer-to-peer sync, a replay that has to reproduce exactly, a
// fixed-step physics tick. Pause, scale and tic size all mean the same things
// they do on a clock of seconds; a tic size of 2 is one tic every second
// iteration.
//
// Atomic because the loop thread advances it while other threads may be
// reading a Timeline anchored to it. Relaxed ordering is enough: there is one
// writer and the count only ever climbs, so no reader can see it go backwards.
class FrameClock final : public TimeSource {
public:
    std::int64_t now() const override { return iterations_.load(std::memory_order_relaxed); }

    void advance() { iterations_.fetch_add(1, std::memory_order_relaxed); }

private:
    std::atomic<std::int64_t> iterations_{0};
};

// A clock a test drives by hand: now() changes only when advance() is called,
// so a test can step time in exact amounts without ever sleeping.
class ManualClock final : public TimeSource {
public:
    std::int64_t now() const override { return now_; }

    void advance(std::int64_t delta) { now_ += delta; }

private:
    std::int64_t now_ = 0;
};
