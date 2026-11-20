#include "PirateCannonsRules.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace PirateCannons {

namespace {

constexpr float degrees = 3.14159265F / 180.0F;

} // namespace

float shipX(Side side)
{
    return side == Side::Player ? playerShipX : cpuShipX;
}

Rect shipBounds(Side side)
{
    return {shipX(side), shipY, shipWidth, shipHeight};
}

Rect islandBounds()
{
    return {islandX, islandY, islandWidth, islandHeight};
}

float muzzleX(Side side)
{
    return side == Side::Player ? playerShipX + shipWidth - 24.0F : cpuShipX + 24.0F - ballSize;
}

float muzzleY()
{
    return shipY - ballSize - 2.0F;
}

Velocity launchVelocity(Shot shot, Side shooter)
{
    const float speed = shot.power;
    const float facing = shooter == Side::Player ? 1.0F : -1.0F;
    return {facing * speed * std::cos(shot.angle * degrees), -speed * std::sin(shot.angle * degrees)};
}

Shot clampShot(Shot shot)
{
    return {std::clamp(shot.angle, minAngle, maxAngle), std::clamp(shot.power, minPower, maxPower)};
}

Flight simulate(float x, float y, Velocity velocity, float wind, float stopY, float dt)
{
    const Rect island = islandBounds();
    // Long enough for any flight across the screen; off-screen ends it early.
    for (int step = 0; step < 4000; ++step) {
        velocity.x += wind * dt;
        velocity.y += gravity * dt;
        x += velocity.x * dt;
        y += velocity.y * dt;
        if (Collision::intersects({x, y, ballSize, ballSize}, island)) {
            return {x, y, true};
        }
        if ((velocity.y > 0.0F && y + ballSize >= stopY) || x < -200.0F || x > width + 200.0F) {
            break;
        }
    }
    return {x, y, false};
}

Shot aimAt(Side shooter, float targetX, float wind)
{
    Shot best;
    float bestMiss = std::numeric_limits<float>::max();
    for (float angle = 20.0F; angle <= 75.0F; angle += 1.0F) {
        for (float power = minPower; power <= maxPower; power += 10.0F) {
            const Shot shot{angle, power};
            const Flight flight =
                simulate(muzzleX(shooter), muzzleY(), launchVelocity(shot, shooter), wind, shipY + 8.0F);
            const float miss = std::abs(flight.x + ballSize * 0.5F - targetX);
            if (!flight.hitIsland && miss < bestMiss) {
                bestMiss = miss;
                best = shot;
            }
        }
    }
    return best;
}

float aimError(int misses)
{
    return std::max(cpuMinError, cpuStartError - cpuErrorShrink * static_cast<float>(std::max(misses, 0)));
}

float rollWind(float unit)
{
    return maxWind * (2.0F * std::clamp(unit, 0.0F, 1.0F) - 1.0F);
}

} // namespace PirateCannons
