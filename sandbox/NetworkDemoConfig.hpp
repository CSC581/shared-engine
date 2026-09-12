#pragma once

#include "NetworkServer.hpp"

#include <cstddef>
#include <cstdint>

// These are choices made by the standalone Section 2 demonstration, not by
// the reusable networking module. A future game supplies its own world setup.
namespace NetworkDemo {

constexpr int windowWidth = 960;
constexpr int windowHeight = 540;
constexpr float arenaX = 32.0F;
constexpr float arenaY = 56.0F;
constexpr float arenaWidth = 896.0F;
constexpr float arenaHeight = 452.0F;
constexpr float playerSize = 32.0F;

struct Color {
    std::uint8_t red;
    std::uint8_t green;
    std::uint8_t blue;
};

inline Color colorForPlayer(Network::PlayerId playerId)
{
    constexpr Color colors[] = {
        {80, 220, 255}, {255, 110, 130}, {255, 210, 80}, {150, 240, 140},
        {190, 140, 255}, {255, 160, 90}, {100, 170, 255}, {245, 120, 220},
    };
    return colors[(playerId - 1) % (sizeof(colors) / sizeof(colors[0]))];
}

inline Network::ServerConfig makeServerConfig()
{
    Network::ServerConfig config;
    config.arenaWidth = arenaWidth;
    config.arenaHeight = arenaHeight;
    config.playerSize = playerSize;
    config.playerSpeed = 220.0F;
    config.spawnPoints = {
        {48.0F, 48.0F}, {309.0F, 48.0F}, {570.0F, 48.0F}, {832.0F, 48.0F},
        {48.0F, 372.0F}, {309.0F, 372.0F}, {570.0F, 372.0F}, {832.0F, 372.0F},
    };
    return config;
}

} // namespace NetworkDemo
