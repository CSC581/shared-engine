#include "TimeSource.hpp"

#include <chrono>

std::int64_t RealTimeClock::now() const
{
    // steady_clock, not system_clock: it is the one standard clock guaranteed
    // never to jump backwards when the machine's wall clock is corrected, and
    // a clock that jumped backwards would break every timeline anchored to it.
    const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
}
