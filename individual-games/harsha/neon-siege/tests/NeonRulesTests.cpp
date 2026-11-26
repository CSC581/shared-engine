#include "NeonRules.hpp"
#include <iostream>

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; ++failures; } } while (false)
void ticks(Neon::Rules& rules, int count) { while (count-- > 0) rules.tick(); }
}
int main()
{
    using namespace Neon;
    Rules rules;
    CHECK(rules.wave() == 1 && rules.lives() == 3);
    CHECK(rules.attemptShot()); CHECK(!rules.attemptShot());
    ticks(rules, 35); CHECK(!rules.attemptShot());
    ticks(rules, 1); CHECK(rules.attemptShot());
    CHECK(rules.shield()); CHECK(!rules.shield()); CHECK(!rules.damage());
    ticks(rules, Rules::ShieldTicks - 1); CHECK(rules.protectedNow());
    ticks(rules, 1); CHECK(!rules.protectedNow()); CHECK(rules.damage());
    CHECK(rules.lives() == 2); CHECK(!rules.damage());
    ticks(rules, 239); CHECK(!rules.damage());
    ticks(rules, 1); CHECK(rules.damage()); CHECK(rules.lives() == 1);
    ticks(rules, Rules::ShieldCooldown - Rules::ShieldTicks - 240 - 1); CHECK(!rules.shield());
    ticks(rules, 1); CHECK(rules.shield());
    rules.clearWave(); CHECK(rules.wave() == 2);
    rules.clearWave(); CHECK(rules.wave() == 3);
    CHECK(rules.pattern() == Pattern::Straight);
    ticks(rules, 1200); CHECK(rules.pattern() == Pattern::Spread);
    ticks(rules, 1200); CHECK(rules.pattern() == Pattern::Spiral);
    ticks(rules, 1200); CHECK(rules.pattern() == Pattern::Straight);
    rules.scoreHit(false); rules.scoreHit(true); CHECK(rules.score() == 110);
    rules.clearWave(); CHECK(rules.phase() == Phase::Won);
    CHECK(!rules.damage() && !rules.shield() && !rules.attemptShot());
    rules.breach(); CHECK(rules.phase() == Phase::Won);
    Rules dead;
    dead.clearWave(); dead.clearWave(); dead.breach(); dead.clearWave();
    CHECK(dead.phase() == Phase::Lost);
    Rules health;
    for (int i = 0; i < 3; ++i) { CHECK(health.damage()); ticks(health, 240); }
    CHECK(health.lives() == 0 && health.phase() == Phase::Lost);
    std::cout << "Neon rules: " << failures << " failures\n";
    return failures != 0;
}
