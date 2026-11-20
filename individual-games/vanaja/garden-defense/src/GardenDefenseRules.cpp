#include "GardenDefenseRules.hpp"

#include <algorithm>

namespace GardenDefense {

int pointsForRow(int row)
{
    return 10 * (rows - std::clamp(row, 0, rows - 1));
}

Color pestColor(int row)
{
    switch (std::clamp(row, 0, rows - 1)) {
    case 0:
        return {150, 110, 230};
    case 1:
        return {120, 205, 90};
    default:
        return {240, 160, 70};
    }
}

March marchStep(float left, float right, float direction, float speed, float dt)
{
    // A paused frame must not step down again at an edge it already reached.
    if (dt <= 0.0F) {
        return {0.0F, 0.0F, direction};
    }
    March march{direction * speed * dt, 0.0F, direction};
    const float minX = edgeMargin;
    const float maxX = static_cast<float>(width) - edgeMargin;
    if (direction > 0.0F && right + march.dx >= maxX) {
        march.dx = std::max(0.0F, maxX - right);
        march.dy = stepDown;
        march.direction = -1.0F;
    } else if (direction < 0.0F && left + march.dx <= minX) {
        march.dx = std::min(0.0F, minX - left);
        march.dy = stepDown;
        march.direction = 1.0F;
    }
    return march;
}

float marchSpeed(int alive, int total, int wave)
{
    const float fallen = total > 0 ? 1.0F - static_cast<float>(std::clamp(alive, 0, total)) / total : 0.0F;
    const float speed = baseMarchSpeed * (1.0F + 3.0F * fallen);
    return wave >= 2 ? speed * waveTwoSpeedUp : speed;
}

float seedInterval(int wave, float unit)
{
    const float seconds = seedIntervalMin + (seedIntervalMax - seedIntervalMin) * std::clamp(unit, 0.0F, 1.0F);
    return wave >= 2 ? seconds * 0.7F : seconds;
}

} // namespace GardenDefense
