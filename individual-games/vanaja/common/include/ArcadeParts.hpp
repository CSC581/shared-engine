#pragma once

#include "Component.hpp"
#include "GameObject.hpp"
#include "TimeSource.hpp"
#include "TimeUnits.hpp"
#include "World.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <thread>

// Small pieces Garden Defense and Pirate Cannons both use. SDL-free, so each
// game's rules library and tests can include them.
namespace Arcade {

// Removes its object after `seconds` of game time. For short-lived cosmetic
// bits such as splats and splinters.
class Lifetime : public Component {
public:
    Lifetime(World& world, float seconds) : world_(world), remaining_(seconds) {}

    void update(const FrameTime& time) override
    {
        remaining_ -= static_cast<float>(time.dtSeconds);
        if (remaining_ <= 0.0F && owner().isActive()) {
            owner().setActive(false);
            world_.destroy(owner().id());
        }
    }

private:
    World& world_;
    float remaining_;
};

// Whether a cosmetic object may take a slot: only while more than
// `gameplayReserve` slots of the world's pool are free, so gameplay objects
// always fit. A heap-backed world has no limit.
inline bool cosmeticRoom(const World& world, std::size_t gameplayReserve)
{
    const std::size_t capacity = world.objectCapacity();
    return capacity == 0 || world.objects().size() + gameplayReserve < capacity;
}

// The engine reports a chord whenever all its keys are held, however long
// ago each went down. A chord counts as pressed together only when its keys'
// press times (in seconds) all fall within this window, so turning around
// with Left still held, or a Space still held from the last shot, does not
// set it off.
constexpr double chordWindowSeconds = 0.2;

inline bool pressedTogether(std::initializer_list<double> pressTimes, double window = chordWindowSeconds)
{
    if (pressTimes.size() == 0) {
        return false;
    }
    const auto [earliest, latest] = std::minmax(pressTimes);
    return latest - earliest <= window;
}

// Engine::run has no frame cap. Called once per frame from handleInput, this
// sleeps until the frame's slot on a fixed schedule, so small oversleeps do
// not add up; a frame more than one interval late restarts the schedule
// instead of rushing to catch up.
class FramePacer {
public:
    explicit FramePacer(std::int64_t hz) : interval_(kNsPerSec / hz) {}

    void wait(const TimeSource& realTime)
    {
        const std::int64_t now = realTime.now();
        if (next_ == 0 || now - next_ > interval_) {
            next_ = now;
        } else if (now < next_) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(next_ - now));
        }
        next_ += interval_;
    }

private:
    std::int64_t interval_;
    std::int64_t next_ = 0;
};

} // namespace Arcade
