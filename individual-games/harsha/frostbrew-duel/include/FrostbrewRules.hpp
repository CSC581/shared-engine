#pragma once

#include <array>
#include <cstddef>

namespace Frostbrew {

enum class Ingredient { Ember, Stone, Growth };
enum class Potion { Blast, Shatter, Bloom, Deep, Earth, Garden };
enum class Phase { Aim, Resolving, GameOver };

struct Recipe {
    Potion potion;
    const char* name;
    const char* description;
    float width;
    float depth;
    int damage;
    bool growth;
};

const Recipe& recipe(Ingredient first, Ingredient second);
const Recipe& recipe(Potion potion);

class Match {
public:
    Match();
    bool beginShot(Potion potion, bool allocated);
    void damage(std::size_t player, int amount);
    void finishShot();
    void reset();
    bool discovered(std::size_t player, Potion potion) const;
    int health(std::size_t player) const;
    std::size_t player() const;
    Phase phase() const;
    int winner() const;

private:
    std::array<int, 2> health_{};
    std::array<std::array<bool, 6>, 2> discoveries_{};
    std::size_t player_ = 0;
    Phase phase_ = Phase::Aim;
};

} // namespace Frostbrew
