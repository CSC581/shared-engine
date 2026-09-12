#pragma once

#include "TimeSource.hpp"

#include <cstdint>
#include <mutex>

// A clock built on top of another clock.
//
// A Timeline converts its anchor's tics into its own at a fixed ratio -- the
// tic size -- and can be paused, resumed, retic'd and rescaled without its own
// reading ever jumping or running backwards. Anchor one to a RealTimeClock for
// game time; anchor one to another Timeline for a clock that inherits the
// parent's pauses and multiplies its scale (a slow-motion layer, a per-client
// loop speed, a replay running at half rate).
//
// A Timeline holds no per-consumer state. "How much time passed since I last
// looked" belongs to a DeltaTimer, one per consumer, so any number of readers
// -- several subsystems, several threads -- can share one timeline.
//
// Lifetime: the anchor must outlive the timeline. A Timeline stores a
// reference to it and reads it on every now().
//
// Thread safety: every public method may be called from any thread. The anchor
// is read while this timeline's own lock is held, so locks are always taken
// child-to-parent; anchor chains are acyclic, so that order cannot deadlock.
// Never call from a parent timeline into a child.
class Timeline final : public TimeSource {
public:
    // ticSize is anchor tics per tic of this timeline, so a larger tic size
    // means a slower timeline. Throws std::invalid_argument if it is not
    // positive.
    Timeline(const TimeSource& anchor, std::int64_t ticSize);

    Timeline(const Timeline&) = delete;
    Timeline& operator=(const Timeline&) = delete;

    // Tics elapsed on this timeline. Never decreases, whatever is done to the
    // pause state, the tic size or the scale.
    std::int64_t now() const override;

    void pause();
    void unpause();
    void togglePause();
    bool isPaused() const;

    // Changing the rate preserves the current reading exactly: now() returns
    // the same value either side of the call, whether the timeline is running
    // or paused at the time, and a rate changed while paused stays invisible
    // until the unpause.
    //
    // The tic still in flight is carried across as the fraction of a tic it
    // is, not as a count of anchor units -- half way through a tic of 10 is
    // still half way through when the tic becomes 5. Carrying the raw units
    // instead would make the clock lurch forward the moment it sped up, and
    // stall when it slowed down.
    void setTicSize(std::int64_t ticSize);
    std::int64_t ticSize() const;

    // Scale is measured against the tic size the timeline was built with, so
    // 2.0 always means twice the original rate and 0.5 half of it, however
    // many times it has been changed. Throws std::invalid_argument for a scale
    // that is not finite and positive.
    void setScale(double scale);
    double scale() const;

private:
    // Folds the anchor time elapsed so far into accumulated_ and moves origin_
    // up to match, returning the partial tic left over. Precondition: mutex_
    // is held and the timeline is not paused.
    std::int64_t rebaseLocked();

    // std::mutex is not recursive, so no public method may call another while
    // holding the lock; the compound operations go through these instead.
    void pauseLocked();
    void unpauseLocked();
    void setTicSizeLocked(std::int64_t ticSize);

    const TimeSource& anchor_;

    mutable std::mutex mutex_;

    // The tic size the timeline was constructed with. scale() is measured
    // against it, so scaling is always relative to the original rate rather
    // than compounding on the last one.
    std::int64_t baseTicSize_;
    std::int64_t ticSize_;

    // The anchor time this timeline's current rate started counting from, and
    // the tics it had already run up before then.
    std::int64_t origin_;
    std::int64_t accumulated_ = 0;

    // The fraction of a tic still in flight when the pause began, carried
    // across the pause so stopping and starting does not round time away.
    std::int64_t pausedRemainder_ = 0;

    bool paused_ = false;
};
