#include "FrostbrewRules.hpp"

#include <algorithm>
#include <stdexcept>

namespace Frostbrew {
namespace {
constexpr std::array<Recipe, 6> recipes{{
    {Potion::Blast, "BLAST FLASK", "CRATER + SPLASH DAMAGE", 48, 48, 38, false},
    {Potion::Shatter, "SHATTER FLASK", "EIGHT EXPLOSIVE FRAGMENTS", 20, 20, 14, false},
    {Potion::Bloom, "BLOOM BOMB", "EIGHT TERRAIN-GROWING SEEDS", 18, 20, 0, true},
    {Potion::Deep, "DEEP FLASK", "NARROW AND DEEP EXPLOSION", 26, 80, 50, false},
    {Potion::Earth, "EARTHBREW", "RAISE A TALL SNOWBANK", 30, 70, 0, true},
    {Potion::Garden, "GARDEN FLASK", "GROW A WIDE SNOWBANK", 85, 28, 0, true}
}};
std::size_t index(Potion potion)
{
    const auto value = static_cast<unsigned>(potion);
    if (value >= recipes.size()) throw std::invalid_argument("Invalid potion");
    return value;
}
void playerIndex(std::size_t player)
{
    if (player >= 2) throw std::invalid_argument("Invalid player");
}
} // namespace

const Recipe& recipe(Ingredient first, Ingredient second)
{
    const auto a = static_cast<unsigned>(first), b = static_cast<unsigned>(second);
    if (a >= 3 || b >= 3) throw std::invalid_argument("Invalid ingredient");
    constexpr Potion combinations[3][3] = {
        {Potion::Blast, Potion::Shatter, Potion::Bloom},
        {Potion::Shatter, Potion::Deep, Potion::Earth},
        {Potion::Bloom, Potion::Earth, Potion::Garden}
    };
    return recipe(combinations[a][b]);
}
const Recipe& recipe(Potion potion) { return recipes[index(potion)]; }

Match::Match() { reset(); }
void Match::reset()
{
    health_.fill(100);
    for (auto& book : discoveries_) { book.fill(false); book[index(Potion::Blast)] = true; }
    player_ = 0;
    phase_ = Phase::Aim;
}
bool Match::beginShot(Potion potion, bool allocated)
{
    const auto i = index(potion);
    if (phase_ != Phase::Aim || !allocated) return false;
    discoveries_[player_][i] = true;
    phase_ = Phase::Resolving;
    return true;
}
void Match::damage(std::size_t player, int amount)
{
    playerIndex(player);
    if (amount < 0) throw std::invalid_argument("Negative damage");
    if (phase_ == Phase::Resolving) health_[player] -= std::min(health_[player], amount);
}
void Match::finishShot()
{
    if (phase_ != Phase::Resolving) return;
    if (health_[0] == 0 || health_[1] == 0) phase_ = Phase::GameOver;
    else { player_ = 1 - player_; phase_ = Phase::Aim; }
}
bool Match::discovered(std::size_t player, Potion potion) const
{
    playerIndex(player);
    return discoveries_[player][index(potion)];
}
int Match::health(std::size_t player) const { playerIndex(player); return health_[player]; }
std::size_t Match::player() const { return player_; }
Phase Match::phase() const { return phase_; }
int Match::winner() const
{
    if (phase_ != Phase::GameOver || health_[0] == health_[1]) return -1;
    return health_[0] > 0 ? 0 : 1;
}
} // namespace Frostbrew
