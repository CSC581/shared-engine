#include "FrostbrewRules.hpp"
#include "FrostbrewTerrain.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; ++failures; } } while (false)
template<class F> void invalid(F action)
{
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
}
using namespace Frostbrew;

void recipesAndDiscovery()
{
    std::set<Potion> kinds;
    for (int a = 0; a < 3; ++a) for (int b = a; b < 3; ++b) {
        const auto first = static_cast<Ingredient>(a);
        const auto second = static_cast<Ingredient>(b);
        const auto& value = recipe(first, second);
        CHECK(value.potion == recipe(second, first).potion);
        CHECK(&value == &recipe(value.potion));
        kinds.insert(value.potion);
    }
    CHECK(kinds.size() == 6);
    invalid([] { recipe(static_cast<Ingredient>(-1), Ingredient::Ember); });
    invalid([] { recipe(static_cast<Potion>(6)); });
    Match match;
    CHECK(match.health(0) == 100 && match.health(1) == 100);
    CHECK(match.discovered(0, Potion::Blast));
    CHECK(!match.discovered(0, Potion::Shatter));
    CHECK(!match.beginShot(Potion::Shatter, false));
    CHECK(match.phase() == Phase::Aim);
    CHECK(!match.discovered(0, Potion::Shatter));
    CHECK(match.beginShot(Potion::Shatter, true));
    CHECK(match.discovered(0, Potion::Shatter));
    CHECK(!match.discovered(1, Potion::Shatter));
    CHECK(!match.beginShot(Potion::Garden, true));
    match.finishShot();
    CHECK(match.player() == 1);
    match.finishShot();
    CHECK(match.player() == 1);
    CHECK(match.beginShot(Potion::Blast, true));
    match.damage(0, 1000);
    match.damage(1, 1000);
    CHECK(match.phase() == Phase::Resolving);
    match.finishShot();
    CHECK(match.phase() == Phase::GameOver);
    CHECK(match.winner() == -1);
    CHECK(match.health(0) == 0 && match.health(1) == 0);
    CHECK(!match.beginShot(Potion::Blast, true));
    match.reset();
    CHECK(match.player() == 0 && match.phase() == Phase::Aim);
    CHECK(!match.discovered(0, Potion::Shatter));
    match.beginShot(Potion::Blast, true);
    match.damage(1, 100);
    match.finishShot();
    CHECK(match.winner() == 0);
    invalid([&] { match.health(2); });
    invalid([&] { match.discovered(2, Potion::Blast); });
    invalid([&] { match.damage(0, -1); });
}

void terrainShapes()
{
    Terrain terrain(101, 200, 100);
    terrain.edit(50, 100, 10, 20, false);
    CHECK(terrain.heights()[50] == 120);
    CHECK(terrain.heights()[40] == 100);
    CHECK(terrain.heights()[39] == 100);
    CHECK(std::abs(terrain.heights()[55] - (100 + std::sqrt(0.75F) * 20)) < 0.001F);
    Terrain growth(101, 200, 100);
    growth.edit(50, 100, 10, 20, true);
    CHECK(growth.heights()[50] == 80);
    CHECK(growth.heights()[40] == 100);
    CHECK(growth.support(45, 10) == 80);
    CHECK(growth.support(40, 11) == 80);
    CHECK(growth.touches(50, 76, 5));
    CHECK(!growth.touches(50, 74, 5));
    const auto before = growth.heights();
    growth.edit(50, 150, 10, 10, false);
    growth.edit(50, 10, 10, 10, true);
    growth.edit(-100, 100, 1, 1, false);
    growth.edit(1000, 100, 1, 1, true);
    growth.edit(50, 100, 0, 10, true);
    CHECK(growth.heights() == before);
    growth.edit(0, 100, 10, 20, false);
    CHECK(growth.heights().front() == 120);
    growth.edit(100, 100, 10, 20, false);
    CHECK(growth.heights().back() == 120);
    Terrain ridge(101, 200, 120);
    ridge.edit(20, 120, 1, 30, true);
    CHECK(ridge.support(20, 20) == 90);
    CHECK(ridge.support(21, 20) == 120);
}

void terrainBoundsAndValidation()
{
    invalid([] { Terrain value(0, 200, 100); });
    invalid([] { Terrain value(10, 80, 75); });
    invalid([] { Terrain value(10, 200, -1); });
    Terrain terrain(101, 200, 100);
    const auto before = terrain.heights();
    invalid([&] { terrain.edit(50, 100, -1, 1, false); });
    invalid([&] { terrain.edit(std::numeric_limits<float>::infinity(), 100, 1, 1, false); });
    invalid([&] { terrain.support(0, std::numeric_limits<float>::quiet_NaN()); });
    CHECK(terrain.heights() == before);
    terrain.edit(std::numeric_limits<float>::max(), 100, 1, 1, false);
    CHECK(terrain.heights() == before);
    terrain.edit(50, 100, 1000, 1000, false);
    CHECK(terrain.heights()[50] == 184);
    terrain.edit(50, 184, 1000, 1000, true);
    CHECK(terrain.heights()[50] == 64);
    Terrain one(1, 200, 100);
    one.generate(17);
    CHECK(one.heights().size() == 1);
    CHECK(std::isfinite(one.support(0, 1)));
    Terrain first(1200, 800, 500), second(1200, 800, 500), third(1200, 800, 500);
    first.generate(42); second.generate(42); third.generate(43);
    CHECK(first.heights() == second.heights());
    CHECK(first.heights() != third.heights());
    for (int i = 0; i < 100; ++i) {
        const auto old = first.heights();
        const bool growth = (i % 2) == 0;
        const int x = i * 37 % 1200;
        first.edit(static_cast<float>(x), old[x], 40, 30, growth);
        for (std::size_t j = 0; j < old.size(); ++j) {
            CHECK(first.heights()[j] >= 64 && first.heights()[j] <= 784);
            CHECK(growth ? first.heights()[j] <= old[j] : first.heights()[j] >= old[j]);
        }
    }
}
} // namespace

int main()
{
    recipesAndDiscovery(); terrainShapes(); terrainBoundsAndValidation();
    std::cout << "Frostbrew rules: " << failures << " failures\n";
    return failures ? 1 : 0;
}
