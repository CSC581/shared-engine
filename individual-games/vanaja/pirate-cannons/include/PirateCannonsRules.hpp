#pragma once

#include "Collision.hpp"

#include <cstddef>
#include <cstdint>

// Pirate Cannons' numbers and the pure rules behind them: launch velocity,
// the cannonball's flight and the computer's aim. Free of SDL and of World,
// so the tests check them directly.
namespace PirateCannons {

constexpr int width = 960;
constexpr int height = 540;

// The loop runs at 125 Hz: an 8 ms frame is a whole number of nanoseconds.
constexpr std::int64_t loopHz = 125;
constexpr float frameSeconds = 1.0F / static_cast<float>(loopHz);

constexpr float seaY = 420.0F;
constexpr float gravity = 400.0F;

constexpr float shipWidth = 90.0F;
constexpr float shipHeight = 40.0F;
constexpr float shipY = seaY - 36.0F;
constexpr float playerShipX = 60.0F;
constexpr float cpuShipX = width - 60.0F - shipWidth;

constexpr float islandX = 430.0F;
constexpr float islandY = 300.0F;
constexpr float islandWidth = 100.0F;
constexpr float islandHeight = 120.0F;

constexpr float ballSize = 10.0F;

// Aim, in degrees above the horizon towards the other ship, and launch speed.
constexpr float minAngle = 10.0F;
constexpr float maxAngle = 80.0F;
constexpr float minPower = 200.0F;
constexpr float maxPower = 700.0F;
constexpr float startAngle = 45.0F;
constexpr float startPower = 450.0F;
// Per second while Up/Down or Left/Right is held.
constexpr float angleRate = 45.0F;
constexpr float powerRate = 180.0F;

// Sideways acceleration, rolled again for every turn.
constexpr float maxWind = 60.0F;

constexpr int shipHp = 3;
constexpr int roundsToWin = 2;
constexpr float roundBreakSeconds = 3.0F;
constexpr float sinkSeconds = 2.0F;

// The Left + Right + Space chord: a ball that splits in three at its peak.
constexpr int chainShotsPerMatch = 2;
constexpr float chainSpread = 0.18F;

// The computer aims at your ship plus an error that shrinks as it misses.
constexpr float cpuThinkSeconds = 1.2F;
constexpr float cpuStartError = 140.0F;
constexpr float cpuErrorShrink = 35.0F;
constexpr float cpuMinError = 20.0F;

constexpr int gulls = 4;

// Every GameObject in a match lives in one pooled World of this many slots.
// Gameplay objects always fit: two ships, the island, the sea, a round's
// gulls plus the last round's still waiting to be removed, and a chain shot's
// ball with the three it splits into. Splinters, splashes and feathers are
// cosmetic and may only use slots beyond that reserve.
constexpr std::size_t objectCapacity = 96;
constexpr std::size_t gameplayReserve = 4 + 2 * gulls + 4;
static_assert(gameplayReserve < objectCapacity, "the pool must hold every gameplay object");

// Collider::layers bits. A cannonball carries them all; nothing else shares
// a bit, so the only collisions reported are the ball's.
constexpr std::uint32_t shipLayer = 1U << 0U;
constexpr std::uint32_t islandLayer = 1U << 1U;
constexpr std::uint32_t seaLayer = 1U << 2U;
constexpr std::uint32_t gullLayer = 1U << 3U;
constexpr std::uint32_t ballLayers = shipLayer | islandLayer | seaLayer | gullLayer;

enum class Side { Player = 0, Cpu = 1 };

constexpr Side other(Side side) { return side == Side::Player ? Side::Cpu : Side::Player; }
constexpr std::size_t index(Side side) { return static_cast<std::size_t>(side); }

struct Shot {
    float angle = startAngle;
    float power = startPower;
};

struct Velocity {
    float x = 0.0F;
    float y = 0.0F;
};

float shipX(Side side);
Rect shipBounds(Side side);
Rect islandBounds();

// Where a side's cannonball starts: just above its own ship, by the cannon.
float muzzleX(Side side);
float muzzleY();

// The player fires to the right, the computer to the left.
Velocity launchVelocity(Shot shot, Side shooter);

Shot clampShot(Shot shot);

// A cannonball's flight in fixed steps, in the order the World runs it: wind
// (a Behavior), then Gravity, then Motion. It ends where the ball comes down
// to `stopY` or meets the island.
struct Flight {
    float x = 0.0F;
    float y = 0.0F;
    bool hitIsland = false;
};
Flight simulate(float x, float y, Velocity velocity, float wind, float stopY, float dt = frameSeconds);

// The shot whose ball comes down nearest `targetX` at ship height without
// meeting the island, from a search over angles and powers.
Shot aimAt(Side shooter, float targetX, float wind);

// How far off the computer aims after `misses` misses in a row.
float aimError(int misses);

// Wind for a turn; `unit` in [0, 1) picks within +-maxWind.
float rollWind(float unit);

// Space fires a plain shot unless Left and Right are both held: then it
// completes the chain-shot chord instead.
bool plainShotAllowed(bool leftHeld, bool rightHeld);

} // namespace PirateCannons
