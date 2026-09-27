#pragma once

#include "Entity.hpp"
#include "NetworkServer.hpp"

#include <array>

namespace SignalBloom {

constexpr int width = 960;
constexpr int height = 540;
constexpr float playerWidth = 30.0F;
constexpr float playerHeight = 40.0F;
constexpr float playerSpeed = 260.0F;
constexpr float jumpSpeed = 480.0F;
constexpr float gravity = 980.0F;

// The floor makes missed jumps recoverable; the two shuttles bridge the climb.
constexpr std::array<Rect, 4> decks{{
    {0.0F, 500.0F, 960.0F, 40.0F},
    {50.0F, 420.0F, 180.0F, 24.0F},
    {510.0F, 340.0F, 150.0F, 24.0F},
    {815.0F, 260.0F, 125.0F, 24.0F},
}};

constexpr std::array<Rect, 3> relayPads{{
    {125.0F, 414.0F, 50.0F, 6.0F},
    {555.0F, 334.0F, 50.0F, 6.0F},
    {852.0F, 254.0F, 50.0F, 6.0F},
}};

constexpr std::array<Network::SpawnPoint, 8> spawns{{
    {76.0F, 380.0F}, {112.0F, 380.0F}, {182.0F, 380.0F},
    {56.0F, 460.0F}, {290.0F, 460.0F}, {450.0F, 460.0F},
    {640.0F, 460.0F}, {810.0F, 460.0F},
}};

constexpr std::array<Network::PlatformPath, 2> shuttlePaths{{
    {1, 245.0F, 385.0F, 420.0F, 385.0F, 95.0F, 112.0F, 18.0F},
    {2, 640.0F, 295.0F, 750.0F, 295.0F, 80.0F, 100.0F, 18.0F},
}};

inline Network::ServerConfig serverConfig()
{
    Network::ServerConfig config;
    config.spawnPoints.assign(spawns.begin(), spawns.end());
    config.maxPlayers = 8;
    config.platforms.assign(shuttlePaths.begin(), shuttlePaths.end());
    return config;
}

} // namespace SignalBloom
