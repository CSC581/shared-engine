#include "DeltaTimer.hpp"

#include <stdexcept>

namespace {

std::int64_t validateMaxDelta(std::int64_t maxDelta)
{
    if (maxDelta <= 0) {
        throw std::invalid_argument("DeltaTimer max delta must be positive");
    }

    return maxDelta;
}

} // namespace

DeltaTimer::DeltaTimer(const TimeSource& source, std::int64_t maxDelta)
    : source_(source),
      last_(source.now()),
      maxDelta_(validateMaxDelta(maxDelta))
{
}

std::int64_t DeltaTimer::tick()
{
    const std::int64_t nowTics = source_.now();

    // last_ moves to the present whether or not the delta was clamped, which
    // is what makes the dropped time dropped rather than deferred.
    std::int64_t delta = nowTics - last_;
    last_ = nowTics;

    if (delta < 0) {
        delta = 0;
    }

    lastWasClamped_ = delta > maxDelta_;

    if (lastWasClamped_) {
        delta = maxDelta_;
    }

    return delta;
}

void DeltaTimer::reset()
{
    // Only the reference point moves; lastWasClamped_ keeps describing the
    // most recent tick(), which a reset is not.
    last_ = source_.now();
}

void DeltaTimer::setMaxDelta(std::int64_t maxDelta)
{
    maxDelta_ = validateMaxDelta(maxDelta);
}

std::int64_t DeltaTimer::maxDelta() const
{
    return maxDelta_;
}

bool DeltaTimer::lastWasClamped() const
{
    return lastWasClamped_;
}
