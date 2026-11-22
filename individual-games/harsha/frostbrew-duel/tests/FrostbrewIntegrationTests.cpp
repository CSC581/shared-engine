#include "FrostbrewArena.hpp"
#include "Components.hpp"
#include "TimeSource.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace Frostbrew;
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; ++failures; } } while (false)
struct Fixture {
    ManualClock clock;
    EventManager events{clock};
    Arena arena;
    explicit Fixture(std::size_t capacity = 128) : arena(events, capacity) {}
    void tick(double dt = 1.0 / 60.0) {
        clock.advance(static_cast<std::int64_t>(dt * 1000000));
        arena.update(FrameTime{dt, clock.now()});
        events.dispatch();
    }
    void finish() {
        int frames = 0;
        while (arena.match().phase() == Phase::Resolving && frames++ < 1200) tick();
        CHECK(arena.match().phase() != Phase::Resolving);
        CHECK(arena.projectileCount() == 0);
    }
};

void rejectedLaunchAndReuse()
{
    Fixture f(2);
    CHECK(f.arena.world().objectCapacity() == 2);
    CHECK(!f.arena.fire(Ingredient::Ember, Ingredient::Stone));
    CHECK(f.arena.match().phase() == Phase::Aim);
    CHECK(!f.arena.match().discovered(0, Potion::Shatter));
    Fixture minimal(3);
    for (int i = 0; i < 12; ++i) {
        CHECK(minimal.arena.fire(Ingredient::Ember, Ingredient::Stone));
        CHECK(!minimal.arena.fire(Ingredient::Ember, Ingredient::Ember));
        minimal.finish();
        minimal.arena.reset();
        CHECK(minimal.arena.world().objects().size() == 2);
    }
}

void defaultPoolHasHeadroom()
{
    for (const auto ingredient : {Ingredient::Stone, Ingredient::Growth}) {
        Fixture f;
        // Occupy cosmetic slots before a split to exercise the maximum live population.
        for (int i = 0; i < 63; ++i) f.arena.world().create("particle");
        CHECK(f.arena.fire(Ingredient::Ember, ingredient));
        auto* body = f.arena.world().findByTag("flask")->get<Transform>();
        body->x = 595; body->y = f.arena.terrain().support(595, 10);
        f.tick(Arena::Step);
        CHECK(f.arena.world().objectCapacity() == 128);
        CHECK(f.arena.projectileCount() == 8);
        CHECK(f.arena.world().findAllByTag("particle").size() == 64);
        CHECK(f.arena.world().objects().size() == 74);
        f.finish();
        f.arena.reset();
        CHECK(f.arena.world().objects().size() == 2);
    }
}

void predictionAndPartitioning()
{
    Fixture a, b;
    a.arena.aim(55, 600); b.arena.aim(55, 600);
    const auto predicted = a.arena.preview();
    CHECK(predicted.size() > 20);
    const auto shortPath = a.arena.preview(120);
    CHECK(shortPath.size() == 121);
    CHECK(std::abs(shortPath.back().x - predicted[120].x) < 0.001F);
    CHECK(a.arena.world().objects().size() == 2);
    CHECK(a.arena.fire(Ingredient::Ember, Ingredient::Ember));
    CHECK(b.arena.fire(Ingredient::Ember, Ingredient::Ember));
    a.tick(0.025);
    for (int i = 0; i < 6; ++i) b.tick(Arena::Step);
    const auto* ta = a.arena.world().findByTag("flask")->get<Transform>();
    const auto* tb = b.arena.world().findByTag("flask")->get<Transform>();
    CHECK(std::abs(ta->x - tb->x) < 0.001F);
    CHECK(std::abs(ta->y - tb->y) < 0.001F);
    CHECK(std::abs(ta->x + 5 - predicted[6].x) < 0.001F);
    CHECK(std::abs(ta->y + 5 - predicted[6].y) < 0.001F);
    const float y = ta->y;
    a.tick(0);
    CHECK(a.arena.world().findByTag("flask")->get<Transform>()->y == y);
    a.finish(); b.finish();
    CHECK(a.arena.terrain().heights() == b.arena.terrain().heights());
}

void allRecipesAndRestart()
{
    Fixture f;
    for (int a = 0; a < 3; ++a) for (int b = a; b < 3; ++b) {
        f.arena.reset();
        f.arena.aim(30, 400);
        const auto before = f.arena.terrain().heights();
        CHECK(f.arena.fire(static_cast<Ingredient>(a), static_cast<Ingredient>(b)));
        f.finish();
        CHECK(f.arena.terrain().heights() != before);
        CHECK(f.arena.match().player() == 1);
        for (int i = 0; i < 120; ++i) f.tick();
        CHECK(f.arena.world().objects().size() == 2);
    }
    CHECK(f.arena.fire(Ingredient::Ember, Ingredient::Stone));
    const auto oldShot = f.arena.shot();
    const auto oldId = f.arena.world().findByTag("flask")->id();
    f.events.raise(Event("Frostbrew.Impact").set("object", oldId).set("shot", oldShot).set("generation", 0));
    f.arena.reset();
    f.events.dispatch();
    CHECK(f.arena.match().health(0) == 100 && f.arena.match().health(1) == 100);
    CHECK(f.arena.world().objects().size() == 2);
    CHECK(f.arena.match().player() == 0);
}

void fragmentsRemainInFlight()
{
    Fixture f;
    bool split = false;
    const auto subscription = f.events.subscribe("Frostbrew.Impact", [&](const Event&) {
        if (f.arena.projectileCount() > 1) split = true;
    });
    f.arena.aim(30, 400);
    CHECK(f.arena.fire(Ingredient::Ember, Ingredient::Stone));
    for (int i = 0; i < 600 && !split; ++i) f.tick();
    CHECK(split);
    f.tick();
    CHECK(f.arena.projectileCount() > 1);
    CHECK(f.arena.match().phase() == Phase::Resolving);
    CHECK(f.arena.match().player() == 0);
    f.finish();
    CHECK(f.arena.match().player() == 1);
    f.events.unsubscribe(subscription);
}

void queuePressureCannotStrandAProjectile()
{
    for (const bool ground : {false, true}) {
        Fixture f;
        f.arena.terrain() = Terrain(Arena::Width, Arena::Height, ground ? 300 : 600);
        auto* first = f.arena.world().find(f.arena.penguin(0))->get<Transform>();
        auto* second = f.arena.world().find(f.arena.penguin(1))->get<Transform>();
        first->x = 200; first->y = 568;
        second->x = ground ? 900 : 248; second->y = 568;
        f.arena.aim(5, 700);
        CHECK(f.arena.fire(Ingredient::Ember, Ingredient::Ember));
        f.events.setMaxPending(1);
        f.events.raise(Event("Unrelated"), 100000000);
        for (int i = 0; i < 6; ++i) f.tick();
        CHECK(f.arena.projectileCount() == 0);
        CHECK(f.arena.match().phase() != Phase::Resolving);
        if (!ground) CHECK(f.arena.match().health(1) < 100);
    }
}

void fastShotsAndExpiry()
{
    Fixture f;
    f.arena.terrain() = Terrain(Arena::Width, Arena::Height, 600);
    auto* first = f.arena.world().find(f.arena.penguin(0))->get<Transform>();
    auto* second = f.arena.world().find(f.arena.penguin(1))->get<Transform>();
    first->x = 200; first->y = 568;
    second->x = 280; second->y = 568;
    f.arena.aim(5, 700);
    CHECK(f.arena.fire(Ingredient::Ember, Ingredient::Ember));
    auto* flask = f.arena.world().findByTag("flask");
    flask->get<Motion>()->velocityX = 100000;
    flask->get<Motion>()->velocityY = 0;
    flask->get<Gravity>()->acceleration = 0;
    f.tick(Arena::Step);
    CHECK(f.arena.match().health(1) < 100);
    CHECK(f.arena.projectileCount() == 0);
    f.arena.reset();
    CHECK(f.arena.fire(Ingredient::Ember, Ingredient::Ember));
    flask = f.arena.world().findByTag("flask");
    flask->get<Transform>()->x = 500; flask->get<Transform>()->y = -100;
    flask->get<Motion>()->velocityX = 0; flask->get<Motion>()->velocityY = 0;
    flask->get<Gravity>()->acceleration = 0;
    for (int i = 0; i < 470; ++i) f.tick();
    CHECK(f.arena.match().phase() == Phase::Resolving);
    for (int i = 0; i < 20; ++i) f.tick();
    CHECK(f.arena.projectileCount() == 0);
    CHECK(f.arena.match().phase() == Phase::Aim);
    CHECK(f.arena.match().player() == 1);
}

void collisionAndCompleteMatch()
{
    Fixture f;
    f.arena.terrain() = Terrain(Arena::Width, Arena::Height, 600);
    auto* first = f.arena.world().find(f.arena.penguin(0))->get<Transform>();
    auto* second = f.arena.world().find(f.arena.penguin(1))->get<Transform>();
    first->x = 200; first->y = 568;
    second->x = 280; second->y = 568;
    int collisions = 0;
    const auto subscription = f.events.subscribe(Events::Collision, [&](const Event& e) {
        if (e.get<std::string>("phase") == "enter") ++collisions;
    });
    f.arena.aim(5, 700);
    CHECK(f.arena.fire(Ingredient::Ember, Ingredient::Ember));
    f.finish();
    CHECK(collisions > 0);
    CHECK(f.arena.match().health(1) < 100);
    f.events.unsubscribe(subscription);
    for (int turn = 0; turn < 30 && f.arena.match().phase() != Phase::GameOver; ++turn) {
        const auto targetPlayer = 1 - f.arena.match().player();
        const auto* target = f.arena.world().find(f.arena.penguin(targetPlayer))->get<Transform>();
        float best = std::numeric_limits<float>::max(), angle = 45, power = 500;
        for (int degrees = 5; degrees <= 85; degrees += 5) for (int strength = 200; strength <= 700; strength += 50) {
            f.arena.aim(static_cast<float>(degrees), static_cast<float>(strength));
            const auto path = f.arena.preview();
            const auto end = path.back();
            const float distance = std::hypot(end.x - (target->x + 16), end.y - (target->y + 16));
            if (distance < best) { best = distance; angle = degrees; power = strength; }
        }
        f.arena.aim(angle, power);
        CHECK(f.arena.fire(Ingredient::Ember, Ingredient::Ember));
        f.finish();
    }
    CHECK(f.arena.match().phase() == Phase::GameOver);
    CHECK(f.arena.match().health(0) == 0 || f.arena.match().health(1) == 0);
    CHECK(!f.arena.fire(Ingredient::Ember, Ingredient::Ember));
    f.arena.reset();
    CHECK(f.arena.match().phase() == Phase::Aim);
}
} // namespace
int main()
{
    rejectedLaunchAndReuse(); defaultPoolHasHeadroom(); predictionAndPartitioning(); allRecipesAndRestart(); fragmentsRemainInFlight();
    queuePressureCannotStrandAProjectile(); fastShotsAndExpiry(); collisionAndCompleteMatch();
    std::cout << "Frostbrew integration: " << failures << " failures\n";
    return failures ? 1 : 0;
}
