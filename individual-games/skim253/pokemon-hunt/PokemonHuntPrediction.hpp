#pragma once

#include "Multiplayer.hpp"
#include "PokemonHuntWorld.hpp"
#include "TimeUnits.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace PokemonHunt {

// Moves Mewtwo and its shadow balls between snapshots, which arrive only as
// often as this client sends (game-time paced), with the server's own rules.
// The next snapshot corrects any wrong guess.
class WorldPredictor {
public:
    // Takes the snapshot only when it changed; `now` is real-time nanoseconds.
    void receive(const std::vector<Multiplayer::Platform>& world, std::int64_t now)
    {
        if (sameWorld(world, latest_)) {
            return;
        }
        const double gap = received_ ? secondsBetween(receivedAt_, now) : 0.0;
        const Multiplayer::Platform* oldBoss = findBoss(latest_);
        const Multiplayer::Platform* newBoss = findBoss(world);

        std::map<std::uint32_t, float> ballDirections;
        const float bossDirection = newBoss != nullptr ? facing(*newBoss) : 1.0F;
        for (const Multiplayer::Platform& object : world) {
            if (isShadowBall(object.id)) {
                ballDirections[object.id] = ballDirection(object, bossDirection);
            }
        }
        bossVelocityY_ = newBoss != nullptr ? estimateVelocityY(oldBoss, *newBoss, gap) : 0.0F;

        latest_ = world;
        ballDirections_ = std::move(ballDirections);
        receivedAt_ = now;
        received_ = true;
    }

    std::vector<Multiplayer::Platform> predict(std::int64_t now) const
    {
        const auto t = static_cast<float>(std::min(secondsBetween(receivedAt_, now), maxPredictSeconds));
        std::vector<Multiplayer::Platform> world;
        world.reserve(latest_.size());
        for (Multiplayer::Platform object : latest_) {
            if (isBoss(object.id)) {
                moveBoss(object, t);
            } else if (isShadowBall(object.id)) {
                object.x += ballDirections_.at(object.id) * shadowBallSpeed * t;
                if (object.x + object.width < 0.0F || object.x > levelWidth) {
                    continue;
                }
            }
            world.push_back(object);
        }
        return world;
    }

private:
    // A link that goes quiet should leave Mewtwo standing, not walking off.
    static constexpr double maxPredictSeconds = 0.5;
    static constexpr float bossFloorY = groundY - bossHeight;

    static double secondsBetween(std::int64_t from, std::int64_t to)
    {
        return static_cast<double>(std::max<std::int64_t>(to - from, 0)) / static_cast<double>(kNsPerSec);
    }

    static bool sameWorld(const std::vector<Multiplayer::Platform>& a, const std::vector<Multiplayer::Platform>& b)
    {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                          [](const Multiplayer::Platform& p, const Multiplayer::Platform& q) {
                              return p.id == q.id && p.x == q.x && p.y == q.y && p.width == q.width &&
                                     p.height == q.height;
                          });
    }

    static const Multiplayer::Platform* findBoss(const std::vector<Multiplayer::Platform>& world)
    {
        for (const Multiplayer::Platform& object : world) {
            if (isBoss(object.id)) {
                return &object;
            }
        }
        return nullptr;
    }

    static float facing(const Multiplayer::Platform& boss)
    {
        return ((boss.id - bossIdBase) & bossFacingLeft) != 0 ? -1.0F : 1.0F;
    }

    static bool airborne(float y) { return y < bossFloorY - 0.5F; }

    // A new ball flies the way Mewtwo faces.
    float ballDirection(const Multiplayer::Platform& ball, float bossDirection) const
    {
        for (const Multiplayer::Platform& old : latest_) {
            if (old.id == ball.id && old.x != ball.x) {
                return ball.x < old.x ? -1.0F : 1.0F;
            }
        }
        const auto known = ballDirections_.find(ball.id);
        return known != ballDirections_.end() ? known->second : bossDirection;
    }

    // From two airborne snapshots, or else from the height: a jump or a respawn drop.
    static float estimateVelocityY(const Multiplayer::Platform* old, const Multiplayer::Platform& now, double gap)
    {
        if (!airborne(now.y)) {
            return 0.0F;
        }
        if (old != nullptr && airborne(old->y) && gap > 0.0) {
            const auto dt = static_cast<float>(gap);
            return (now.y - old->y) / dt + bossGravity * dt * 0.5F;
        }
        if (old != nullptr) {
            const float height = bossFloorY - now.y;
            return -std::sqrt(std::max(0.0F, bossJumpSpeed * bossJumpSpeed - 2.0F * bossGravity * height));
        }
        return std::sqrt(2.0F * bossGravity * std::max(0.0F, now.y + bossHeight));
    }

    void moveBoss(Multiplayer::Platform& boss, float t) const
    {
        const bool charging = ((boss.id - bossIdBase) & bossCharging) != 0;
        if (!charging) {
            float x = boss.x + facing(boss) * bossWalkSpeed * t;
            bool turned = false;
            if (x > bossMaxX) {
                x = std::max(bossMinX, 2.0F * bossMaxX - x);
                turned = true;
            } else if (x < bossMinX) {
                x = std::min(bossMaxX, 2.0F * bossMinX - x);
                turned = true;
            }
            boss.x = x;
            if (turned) {
                boss.id = bossIdBase + ((boss.id - bossIdBase) ^ bossFacingLeft);
            }
        }
        if (airborne(boss.y)) {
            boss.y = std::min(bossFloorY, boss.y + bossVelocityY_ * t + 0.5F * bossGravity * t * t);
        }
    }

    std::vector<Multiplayer::Platform> latest_;
    std::map<std::uint32_t, float> ballDirections_;
    float bossVelocityY_ = 0.0F;
    std::int64_t receivedAt_ = 0;
    bool received_ = false;
};

} // namespace PokemonHunt
