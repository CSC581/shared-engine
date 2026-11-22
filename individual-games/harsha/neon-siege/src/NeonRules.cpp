#include "NeonRules.hpp"

namespace Neon {
void Rules::tick()
{
    if (phase_ != Phase::Playing) return;
    if (fireCooldown_ > 0) --fireCooldown_;
    if (shieldCooldown_ > 0) --shieldCooldown_;
    if (shieldTicks_ > 0) --shieldTicks_;
    if (invulnerability_ > 0) --invulnerability_;
    if (wave_ == 3) ++bossTicks_;
}
bool Rules::attemptShot()
{
    if (phase_ != Phase::Playing || fireCooldown_ > 0) return false;
    fireCooldown_ = 36;
    return true;
}
bool Rules::shield()
{
    if (phase_ != Phase::Playing || shieldCooldown_ > 0) return false;
    shieldTicks_ = ShieldTicks; shieldCooldown_ = ShieldCooldown;
    return true;
}
bool Rules::damage()
{
    if (phase_ != Phase::Playing || protectedNow()) return false;
    --lives_; invulnerability_ = 240;
    if (lives_ == 0) phase_ = Phase::Lost;
    return true;
}
void Rules::breach() { if (phase_ == Phase::Playing) phase_ = Phase::Lost; }
void Rules::clearWave()
{
    if (phase_ != Phase::Playing) return;
    if (wave_ < 3) { ++wave_; bossTicks_ = 0; }
    else phase_ = Phase::Won;
}
Phase Rules::phase() const { return phase_; }
int Rules::wave() const { return wave_; }
int Rules::lives() const { return lives_; }
bool Rules::protectedNow() const { return shieldTicks_ > 0 || invulnerability_ > 0; }
int Rules::shieldCooldown() const { return shieldCooldown_; }
int Rules::shieldTicks() const { return shieldTicks_; }
Pattern Rules::pattern() const { return static_cast<Pattern>((bossTicks_ / 1200) % 3); }
void Rules::scoreHit(bool boss) { score_ += boss ? 100 : 10; }
int Rules::score() const { return score_; }
} // namespace Neon
