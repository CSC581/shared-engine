#pragma once

#include "NeonRules.hpp"
#include "NeonStorage.hpp"
#include "FrameTime.hpp"

#include <vector>

namespace Neon {
struct Controls { float horizontal = 0; bool fire = false; bool shield = false; };
class Arena {
public:
    static constexpr int Width = 960, Height = 720;
    static constexpr double Step = 1.0 / Rules::TicksPerSecond;
    explicit Arena(StorageMode mode = StorageMode::Reuse);
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) = delete;
    Arena& operator=(Arena&&) = delete;
    void update(const FrameTime& time, Controls controls = {});
    const Rules& rules() const;
    Storage& storage();
    const Storage& storage() const;
    Handle player() const;
    std::uint64_t digest() const;
private:
    struct Hit { Handle projectile, target; std::uint64_t birth; bool hostile; };
    void loadWave(bool rebuild);
    void step(Controls controls);
    void contact(Handle self, Handle other);
    void resolveHits();
    void burst(float x, float y);
    void enemyFire();
    Storage storage_;
    Rules rules_;
    Handle player_{}, guide_{};
    std::vector<Hit> hits_;
    double accumulator_ = 0;
    std::uint64_t ticks_ = 0;
    int fireTicks_ = 0, direction_ = 1;
    float formationY_ = 110, spiral_ = 0;
    bool transition_ = false, shieldRequested_ = false;
};
} // namespace Neon
