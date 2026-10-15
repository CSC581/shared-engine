#pragma once

#include "PokemonHuntStyle.hpp"
#include "PokemonHuntWorld.hpp"
#include "TimeUnits.hpp"

#include <SDL3/SDL.h>

#include <cstdint>

// Client tuning. Mewtwo's numbers are in PokemonHuntWorld.hpp, shared with the server.
namespace PokemonHunt {

constexpr float pokemonSize = 40.0F;
constexpr float walkSpeed = 240.0F;
constexpr float jumpSpeed = 680.0F;
constexpr float gravity = 1400.0F;
constexpr float stompBounce = 560.0F;

constexpr float spawnFirstX = 80.0F;
constexpr float spawnSpacingX = 240.0F;
constexpr float spawnY = 200.0F;

// The camera scrolls once this Pokemon is this close to a screen edge.
constexpr float scrollMargin = 320.0F;

constexpr float shotSpeed = 560.0F;
constexpr float shotSize = 14.0F;
constexpr double shotCooldown = 0.5;
// Ahead of the Pokemon's centre, at this fraction of its height.
constexpr float shotSpawnOffset = 18.0F;
constexpr float shotSpawnHeightRatio = 0.45F;

constexpr int stompDamage = 3;
constexpr int shotDamage = 1;
constexpr int stompPoints = 100;
constexpr int shotPoints = 20;
constexpr int hurtPoints = 50;

// Hitboxes are trimmed so transparent sprite padding cannot hit.
struct Insets {
    float sides; // left and right each
    float top;
    float bottom;
};
constexpr Insets bossHitInsets{8.0F, 4.0F, 0.0F};
constexpr Insets shadowBallHitInsets{4.0F, 4.0F, 4.0F};
// A stomp still counts with feet this far below Mewtwo's head last step.
constexpr float stompTolerance = 14.0F;
constexpr double stompInvincible = 0.3;

constexpr double hurtInvincible = 1.5;
constexpr double hurtControlLock = 0.35;
constexpr float hurtKnockbackX = 320.0F;
constexpr float hurtKnockbackY = 380.0F;

// Time effects. Durations are real seconds: a frozen timeline cannot time its own end.
constexpr int hasteComboHits = 3;       // hits in a row without getting hurt
constexpr double hasteScale = 2.0;
constexpr double hasteSeconds = 5.0;
constexpr int freezeHurtCount = 2;      // hurt this many times...
constexpr double freezeWindowSeconds = 3.0; // ...within this long
constexpr double freezeSeconds = 1.5;
constexpr std::int64_t effectBannerNs = kNsPerSec;

struct SpeedKey {
    SDL_Scancode key;
    double scale;
};
constexpr SpeedKey speedKeys[] = {
    {SDL_SCANCODE_1, 0.5},
    {SDL_SCANCODE_2, 1.0},
    {SDL_SCANCODE_3, 2.0},
};

constexpr double bossFlashSeconds = 0.12;
constexpr double popupSeconds = 1.0;
constexpr double popupFadeSeconds = 0.5;
constexpr double popupRiseSpeed = 40.0;
constexpr float popupOffsetY = 10.0F;
constexpr Uint64 blinkPeriodMs = 90;
constexpr Uint64 chargeGlowPeriodMs = 100;

constexpr int maxPeerId = 8;
constexpr int firstPeerBasePort = 7300;
constexpr int portsPerPeer = 2;
constexpr const char* firstPeerEndpoint = "tcp://127.0.0.1:7300";
// Give up after the timeout; once joined, listen a moment for every peer's announcement.
constexpr double joinTimeoutSeconds = 3.0;
constexpr double joinSettleSeconds = 1.0;
constexpr Uint32 joinPollMs = 20;

// Sends follow game time (30/s at 1x); a real-time heartbeat keeps a paused client alive.
constexpr std::int64_t sendIntervalGameTics = kGameTicsPerSecond / 30;
constexpr std::int64_t heartbeatIntervalRealTics = 250 * kNsPerMs;

enum SpeciesId { Pikachu, Charmander, Squirtle, Bulbasaur, SpeciesCount };

struct Species {
    const char* name;
    const char* file;
    Color color; // scoreboard dot and skill projectile
    const char* skill;
};

constexpr Species species[SpeciesCount] = {
    {"Pikachu", "pikachu.png", {255, 230, 80}, "Thunderbolt"},
    {"Charmander", "charmander.png", {255, 120, 40}, "Ember"},
    {"Squirtle", "squirtle.png", {110, 190, 255}, "Water Gun"},
    {"Bulbasaur", "bulbasaur.png", {120, 220, 100}, "Razor Leaf"},
};

constexpr const char* bossSpriteFile = "mewtwo.png";
constexpr float pokemonSpriteScale = 1.0F;
constexpr float bossSpriteScale = 2.0F;
constexpr double animationFps = 10.0;

constexpr float spawnX(int kind)
{
    return spawnFirstX + static_cast<float>(kind) * spawnSpacingX;
}

} // namespace PokemonHunt
