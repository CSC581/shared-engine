#pragma once

#include "Color.hpp"

#include <cstddef>
#include <cstdint>

// Garden Defense's numbers and the pure rules behind them. Free of SDL and of
// World, so the tests check them directly.
namespace GardenDefense {

constexpr int width = 800;
constexpr int height = 600;

// The loop runs at 125 Hz: an 8 ms frame is a whole number of nanoseconds.
constexpr std::int64_t loopHz = 125;

// The formation: 4 rows of 8 pests, worth more the higher they march.
constexpr int columns = 8;
constexpr int rows = 4;
constexpr float pestWidth = 34.0F;
constexpr float pestHeight = 24.0F;
constexpr float columnSpacing = 56.0F;
constexpr float rowSpacing = 40.0F;
constexpr float formationLeft =
    (static_cast<float>(width) - (columns - 1) * columnSpacing - pestWidth) * 0.5F;
constexpr float formationTop = 100.0F;
// Wave 2 starts this much lower and marches faster.
constexpr float waveTwoDrop = 40.0F;
constexpr float waveTwoSpeedUp = 1.4F;
constexpr float edgeMargin = 20.0F;
constexpr float stepDown = 18.0F;
// Pixels per second with the whole formation alive; up to 4x with one left.
constexpr float baseMarchSpeed = 40.0F;

// A pest that touches the fence at this height has reached the garden.
constexpr float gardenLineY = 500.0F;

constexpr float canWidth = 44.0F;
constexpr float canHeight = 28.0F;
constexpr float canY = 536.0F;
constexpr float canSpeed = 320.0F;
constexpr int startingLives = 3;
constexpr float respawnSeconds = 1.0F;
constexpr float respawnShieldSeconds = 1.5F;

constexpr float dropWidth = 6.0F;
constexpr float dropHeight = 14.0F;
constexpr float dropSpeed = 520.0F;
constexpr int maxDrops = 3;

constexpr float seedWidth = 6.0F;
constexpr float seedHeight = 10.0F;
constexpr float seedSpeed = 200.0F;
constexpr int maxSeeds = 8;
constexpr float seedIntervalMin = 0.8F;
constexpr float seedIntervalMax = 1.8F;

// The Left + Right chord.
constexpr float umbrellaSeconds = 1.5F;
constexpr float umbrellaCooldown = 6.0F;

constexpr float ladybugY = 56.0F;
constexpr float ladybugWidth = 30.0F;
constexpr float ladybugHeight = 20.0F;
constexpr float ladybugSpeed = 130.0F;
constexpr float ladybugIntervalMin = 20.0F;
constexpr float ladybugIntervalMax = 30.0F;
constexpr int ladybugPoints = 150;

constexpr float waveBreakSeconds = 2.0F;

constexpr int splatsPerPest = 8;
constexpr float splatSeconds = 0.8F;
constexpr int bloomPetals = 120;
constexpr float bloomSeconds = 2.5F;

// Every GameObject in a match lives in one pooled World of this many slots.
// Gameplay objects always fit: the whole formation, the can, the fence, every
// drop and seed allowed at once and a ladybug. Splats and petals are cosmetic
// and may only use slots beyond that reserve, so they are skipped near the
// limit and never crowd out a pest, drop or seed.
constexpr std::size_t objectCapacity = 160;
constexpr std::size_t gameplayReserve =
    static_cast<std::size_t>(rows * columns + maxDrops + maxSeeds) + 3;
static_assert(gameplayReserve < objectCapacity, "the pool must hold every gameplay object");

// Collider::layers bits: what may touch what.
constexpr std::uint32_t sprayLayer = 1U << 0U;  // drops, pests, ladybug
constexpr std::uint32_t seedLayer = 1U << 1U;   // seeds, can
constexpr std::uint32_t gardenLayer = 1U << 2U; // pests, fence

// 40 for the top row down to 10 for the bottom one.
int pointsForRow(int row);

// Beetles on top, then caterpillars, then aphids.
Color pestColor(int row);

// One frame of the formation's march. `left` and `right` are the outer edges
// of the living pests. The formation moves sideways; when that would cross an
// edge margin it stops at the margin, steps down and turns around.
struct March {
    float dx = 0.0F;
    float dy = 0.0F;
    float direction = 1.0F;
};
March marchStep(float left, float right, float direction, float speed, float dt);

// The march speeds up as pests fall, and wave 2 is faster again.
float marchSpeed(int alive, int total, int wave);

// Seconds until the next seed; `unit` in [0, 1) picks within the range.
float seedInterval(int wave, float unit);

} // namespace GardenDefense
