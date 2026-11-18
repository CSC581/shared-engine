#include "FrostbrewArena.hpp"
#include "Components.hpp"
#include "TimeSource.hpp"

#include <iostream>

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; ++failures; } } while (false)

void exercise(std::size_t capacity)
{
    ManualClock clock;
    EventManager events(clock);
    Frostbrew::Arena arena(events, capacity);
    CHECK(arena.poolUsage().peakObjects == 2);
    CHECK(arena.poolUsage().peakProjectiles == 0);
    CHECK(arena.poolUsage().peakParticles == 0);
    arena.aim(30, 400);
    const bool fired = arena.fire(Frostbrew::Ingredient::Ember, Frostbrew::Ingredient::Stone);
    CHECK(fired == (capacity > 2));
    CHECK(arena.poolUsage().primaryRejected == (capacity == 2 ? 1U : 0U));
    for (int frame = 0; arena.match().phase() == Frostbrew::Phase::Resolving && frame < 1200; ++frame) {
        clock.advance(16667);
        arena.update(FrameTime{1.0 / 60.0, clock.now()});
    }
    CHECK(arena.match().phase() != Frostbrew::Phase::Resolving);
    const auto usage = arena.poolUsage();
    CHECK(usage.peakObjects <= capacity);
    CHECK(usage.peakParticles <= 64);
    CHECK(usage.peakProjectiles <= 8);
    if (capacity == 3) {
        CHECK(usage.primarySpawned == 1);
        CHECK(usage.secondarySpawned == 1);
        CHECK(usage.secondaryRejected == 7);
        CHECK(usage.particleReserveSkipped > 0);
        CHECK(usage.peakObjects == 3);
    }
    if (capacity == 128) {
        CHECK(usage.secondarySpawned == 8);
        CHECK(usage.secondaryRejected == 0);
        CHECK(usage.peakProjectiles == 8);
        CHECK(usage.particlesSpawned + usage.particleLimitSkipped +
              usage.particleReserveSkipped + usage.particleRejected == 9 * 12);
    }
    CHECK(usage.particleRejected == 0);
    arena.reset();
    CHECK(arena.world().objects().size() == 2);
    CHECK(arena.poolUsage().peakObjects == usage.peakObjects);
    CHECK(arena.poolUsage().secondaryRejected == usage.secondaryRejected);
}

void particleReserveBoundary(std::size_t capacity)
{
    ManualClock clock;
    EventManager events(clock);
    Frostbrew::Arena arena(events, capacity);
    for (int i = 0; i < 63; ++i) arena.world().create("particle");
    CHECK(arena.fire(Frostbrew::Ingredient::Ember, Frostbrew::Ingredient::Growth));
    auto* flask = arena.world().findByTag("flask");
    auto* body = flask->get<Transform>();
    body->x = 595; body->y = arena.terrain().support(595, 10);
    arena.update(FrameTime{Frostbrew::Arena::Step, clock.now()});
    CHECK(arena.poolUsage().secondarySpawned == 8);
    CHECK(arena.poolUsage().particlesSpawned == (capacity == 83 ? 1U : 0U));
    CHECK(arena.poolUsage().particleReserveSkipped == (capacity == 83 ? 0U : 12U));
    CHECK(arena.poolUsage().particleLimitSkipped == (capacity == 83 ? 11U : 0U));
    CHECK(arena.poolUsage().peakObjects == (capacity == 83 ? 74U : 73U));
}
} // namespace

int main()
{
    for (const std::size_t capacity : {2U, 3U, 128U}) exercise(capacity);
    particleReserveBoundary(82);
    particleReserveBoundary(83);
    std::cout << "Frostbrew pool usage: " << failures << " failures\n";
    return failures ? 1 : 0;
}
