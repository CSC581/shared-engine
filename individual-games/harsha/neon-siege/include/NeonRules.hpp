#pragma once

namespace Neon {
enum class Phase { Playing, Won, Lost };
enum class Pattern { Straight, Spread, Spiral };

class Rules {
public:
    static constexpr int TicksPerSecond = 240;
    void tick();
    bool attemptShot();
    bool shield();
    bool damage();
    void breach();
    void clearWave();
    Phase phase() const;
    int wave() const;
    int lives() const;
    bool protectedNow() const;
    int shieldCooldown() const;
    int shieldTicks() const;
    Pattern pattern() const;
    void scoreHit(bool boss);
    int score() const;
private:
    Phase phase_ = Phase::Playing;
    int wave_ = 1;
    int lives_ = 3;
    int fireCooldown_ = 0;
    int shieldCooldown_ = 0;
    int shieldTicks_ = 0;
    int invulnerability_ = 0;
    int bossTicks_ = 0;
    int score_ = 0;
};
} // namespace Neon
