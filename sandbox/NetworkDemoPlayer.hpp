#pragma once

#include "Entity.hpp"
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
                entity_.setPosition(player.x, player.y);
                entity_.setVelocity(0.0F, 0.0F);
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
        entity_.setVelocity(x * playerSpeed, y * playerSpeed);
        entity_.update(deltaTime);
        entity_.setPosition(std::clamp(entity_.getX(), 0.0F, arenaWidth - playerSize),
                            std::clamp(entity_.getY(), 0.0F, arenaHeight - playerSize));
    }

    Rect bounds() const { return entity_.getBounds(); }

private:
    Network::PlayerId id_ = 0;
    Entity entity_{0.0F, 0.0F, playerSize, playerSize};
};

} // namespace NetworkDemo
