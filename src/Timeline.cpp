#include "Timeline.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {

std::int64_t validateTicSize(std::int64_t ticSize)
{
    if (ticSize <= 0) {
        throw std::invalid_argument("Timeline tic size must be positive");
    }

    return ticSize;
}

// Converts a partial tic from one rate to another.
//
// What is carried across a rate change is a *fraction of a tic*, not a count
// of anchor units. Half way through a tic of 10, a timeline is still half way
// through when the tic becomes 5 -- it is not suddenly a whole tic further on.
// Carrying the raw 5 anchor units across and dividing them by the new tic size
// is what made the clock lurch the instant its rate changed, and stall in the
// other direction when the rate got slower.
std::int64_t rescaleRemainder(std::int64_t remainder, std::int64_t fromTicSize,
                              std::int64_t toTicSize)
{
    if (remainder <= 0 || fromTicSize == toTicSize) {
        return remainder < 0 ? 0 : remainder;
    }

    std::int64_t rescaled = 0;

    if (remainder <= std::numeric_limits<std::int64_t>::max() / toTicSize) {
        rescaled = remainder * toTicSize / fromTicSize;
    } else {
        // Only reachable with tic sizes in the billions of billions, where the
        // exact product would overflow. Precision is worth less there than not
        // wrapping around.
        rescaled = static_cast<std::int64_t>(static_cast<long double>(remainder) /
                                             static_cast<long double>(fromTicSize) *
                                             static_cast<long double>(toTicSize));
    }

    // A partial tic is by definition less than a whole one. The arithmetic
    // above already guarantees it; the clamp makes the invariant that actually
    // matters -- a rate change never advances the clock -- independent of it.
    return std::max<std::int64_t>(0, std::min(rescaled, toTicSize - 1));
}

} // namespace

Timeline::Timeline(const TimeSource& anchor, std::int64_t ticSize)
    : anchor_(anchor),
      baseTicSize_(validateTicSize(ticSize)),
      ticSize_(ticSize),
      origin_(anchor.now())
{
}

std::int64_t Timeline::now() const
{
    const std::lock_guard<std::mutex> lock(mutex_);

    if (paused_) {
        return accumulated_;
    }

    return accumulated_ + (anchor_.now() - origin_) / ticSize_;
}

void Timeline::pause()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    pauseLocked();
}

void Timeline::unpause()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    unpauseLocked();
}

void Timeline::togglePause()
{
    const std::lock_guard<std::mutex> lock(mutex_);

    if (paused_) {
        unpauseLocked();
    } else {
        pauseLocked();
    }
}

bool Timeline::isPaused() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return paused_;
}

void Timeline::setTicSize(std::int64_t ticSize)
{
    // Validated before the lock is taken: a rejected value must leave the
    // timeline exactly as it was.
    validateTicSize(ticSize);

    const std::lock_guard<std::mutex> lock(mutex_);
    setTicSizeLocked(ticSize);
}

std::int64_t Timeline::ticSize() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return ticSize_;
}

void Timeline::setScale(double scale)
{
    if (!std::isfinite(scale) || scale <= 0.0) {
        throw std::invalid_argument("Timeline scale must be finite and positive");
    }

    const std::lock_guard<std::mutex> lock(mutex_);

    const double rawTicSize = static_cast<double>(baseTicSize_) / scale;

    // std::llround is undefined outside int64_t's range, which a scale small
    // enough to overflow the division reaches; saturate instead of tripping
    // over it. A tic size this large is already a stopped clock in practice.
    constexpr double maxTicSize = 9.0e18;

    const std::int64_t ticSize = rawTicSize >= maxTicSize
                                     ? static_cast<std::int64_t>(maxTicSize)
                                     : std::max<std::int64_t>(1, std::llround(rawTicSize));

    setTicSizeLocked(ticSize);
}

double Timeline::scale() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<double>(baseTicSize_) / static_cast<double>(ticSize_);
}

std::int64_t Timeline::rebaseLocked()
{
    const std::int64_t anchorNow = anchor_.now();
    const std::int64_t elapsed = anchorNow - origin_;
    const std::int64_t remainder = elapsed % ticSize_;

    accumulated_ += elapsed / ticSize_;

    // Carry the partial tic instead of discarding it: origin_ moves forward
    // only by the whole tics that were banked, so a run of rebases at one rate
    // loses no time at all. When the rate itself changes, the caller converts
    // this remainder with rescaleRemainder.
    origin_ = anchorNow - remainder;

    return remainder;
}

void Timeline::pauseLocked()
{
    if (paused_) {
        return;
    }

    pausedRemainder_ = rebaseLocked();
    paused_ = true;
}

void Timeline::unpauseLocked()
{
    if (!paused_) {
        return;
    }

    // Resume as though the pause had never happened: back-date the origin by
    // the partial tic that was in flight when it started.
    origin_ = anchor_.now() - pausedRemainder_;
    paused_ = false;
}

void Timeline::setTicSizeLocked(std::int64_t ticSize)
{
    if (paused_) {
        // now() is already frozen at accumulated_, so there is nothing to
        // bank; only the partial tic waiting to be resumed has to be restated
        // at the new rate, or unpausing would credit it as more (or less) than
        // the fraction of a tic it actually is.
        pausedRemainder_ = rescaleRemainder(pausedRemainder_, ticSize_, ticSize);
        ticSize_ = ticSize;
        return;
    }

    // Bank everything earned at the old rate first, so the reading does not
    // jump...
    const std::int64_t remainder = rebaseLocked();

    // ...and restate the tic still in flight at the new rate too. rebaseLocked
    // has just left origin_ at anchorNow - remainder, so anchorNow is recovered
    // from it rather than read again: the anchor may have moved in between, and
    // a second reading would silently drop that sliver of time.
    const std::int64_t anchorNow = origin_ + remainder;
    origin_ = anchorNow - rescaleRemainder(remainder, ticSize_, ticSize);

    ticSize_ = ticSize;
}
