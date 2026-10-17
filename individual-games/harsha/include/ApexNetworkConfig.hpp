#pragma once

#include "NetworkServer.hpp"

#include <cstdint>
#include <vector>

// Shared Apex tower coordinates for the headless server and the game client.
// Moving platforms live in movers() so online poses cannot drift from offline.
namespace ApexNetwork {

constexpr int designWidth = 900;
constexpr int designHeight = 900;
constexpr int roomCount = 6;

constexpr float groundThickness = 140.0F;
constexpr float platformThickness = 60.0F;
constexpr float playerWidth = 50.0F;
constexpr float playerHeight = 70.0F;

inline float worldHeight()
{
    return static_cast<float>(designHeight * roomCount);
}

inline float room0Top()
{
    return worldHeight() - static_cast<float>(designHeight);
}

// How a mover behaves beyond following its path. ApexLevel maps it to an
// ApexPlatform::Kind; kept separate so the headless server, which has no
// client object model, can include this header.
enum class MoverStyle { Plain, Slingshot, Orbit };

struct MoverSpec {
    MoverStyle style;
    // The engine's own path type: the server runs exactly this, and the
    // offline level builds its PathMover from it.
    Network::PlatformPath path;
};

// Every moving platform in the tower. The single list read by the offline
// level, the server config and the online platform factory.
inline const std::vector<MoverSpec>& movers()
{
    static const std::vector<MoverSpec> list = [] {
        std::vector<MoverSpec> specs;

        // Room 0: amber horizontal mover above the hand-placed ledges.
        Network::PlatformPath horizontal;
        horizontal.id = 1;
        horizontal.startX = 300.0F;
        horizontal.startY = room0Top() + 150.0F;
        horizontal.endX = 550.0F;
        horizontal.endY = horizontal.startY;
        horizontal.speed = 150.0F;
        horizontal.width = 150.0F;
        horizontal.height = platformThickness;
        specs.push_back({MoverStyle::Plain, horizontal});

        return specs;
    }();
    return list;
}

// The mover with this network id, or nullptr.
inline const MoverSpec* findMover(std::uint32_t id)
{
    for (const MoverSpec& spec : movers()) {
        if (spec.path.id == id) {
            return &spec;
        }
    }
    return nullptr;
}

inline float startX()
{
    return static_cast<float>(designWidth) * 0.5F - playerWidth * 0.5F;
}

inline float startY()
{
    return worldHeight() - groundThickness - playerHeight;
}

inline Network::ServerConfig makeServerConfig()
{
    Network::ServerConfig config;
    // Room-0 ground spawns (not the arena demo pads).
    config.spawnPoints = {
        {startX(), startY()},
        {startX() + 40.0F, startY()},
        {startX() - 40.0F, startY()},
        {startX() + 80.0F, startY()},
        {startX() - 80.0F, startY()},
        {startX() + 120.0F, startY()},
        {startX() - 120.0F, startY()},
        {startX() + 160.0F, startY()},
    };
    for (const MoverSpec& spec : movers()) {
        config.platforms.push_back(spec.path);
    }
    return config;
}

} // namespace ApexNetwork
