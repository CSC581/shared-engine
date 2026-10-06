#pragma once

#include "Collision.hpp"
#include "NetworkDemoConfig.hpp"

#include <algorithm>
#include <cmath>

namespace NetworkDemo {

// Local character simulation for the arena demo, not a networking rule.
class Player {
public:
    void reset() { id_ = 0; }

    bool synchronize(const Network::WorldSnapshot& snapshot, Network::PlayerId id)
    {
        if (id != 0 && id_ == id) {
            return true;
        }
        for (const auto& player : snapshot.players) {
            if (id != 0 && player.id == id) {
                x_ = player.x;
                y_ = player.y;
                id_ = id;
                return true;
            }
        }
        return false;
    }

    void update(int horizontal, int vertical, float deltaTime)
    {
        if (id_ == 0) {
            return;
        }
        float x = static_cast<float>(std::clamp(horizontal, -1, 1));
        float y = static_cast<float>(std::clamp(vertical, -1, 1));
        const float length = std::sqrt(x * x + y * y);
        if (length > 1.0F) {
            x /= length;
            y /= length;
        }
        x_ = std::clamp(x_ + x * playerSpeed * deltaTime, 0.0F, arenaWidth - playerSize);
        y_ = std::clamp(y_ + y * playerSpeed * deltaTime, 0.0F, arenaHeight - playerSize);
    }

    Rect bounds() const { return { x_, y_, playerSize, playerSize }; }

private:
    Network::PlayerId id_ = 0;
    float x_ = 0.0F;
    float y_ = 0.0F;
};

} // namespace NetworkDemo
