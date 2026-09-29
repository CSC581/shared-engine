#pragma once

#include "Entity.hpp"
#include "NetworkServer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace StarfallSalvage {

constexpr int width = 960;
constexpr int worldWidth = 1600;
constexpr int height = 540;
constexpr float playerWidth = 30.0F;
constexpr float playerHeight = 40.0F;
constexpr float playerSpeed = 260.0F;
constexpr float jumpSpeed = 500.0F;
constexpr float gravity = 900.0F;
constexpr int maximumJumps = 2;

// The floor makes missed jumps recoverable; the two shuttles bridge the climb.
constexpr std::array<Rect, 6> decks{{
    {0.0F, 500.0F, static_cast<float>(worldWidth), 40.0F},
    {50.0F, 420.0F, 180.0F, 24.0F},
    {510.0F, 340.0F, 150.0F, 24.0F},
    {815.0F, 260.0F, 125.0F, 24.0F},
    {1010.0F, 320.0F, 240.0F, 24.0F},
    {1290.0F, 260.0F, 260.0F, 24.0F},
}};

constexpr std::array<Rect, 3> relayPads{{
    {125.0F, 414.0F, 50.0F, 6.0F},
    {555.0F, 334.0F, 50.0F, 6.0F},
    {1390.0F, 254.0F, 50.0F, 6.0F},
}};

constexpr Rect bulkhead{970.0F, -1000.0F, 20.0F, 1500.0F};
constexpr std::array<Rect, 2> gatePlates{{
    {850.0F, 254.0F, 50.0F, 6.0F},
    {1160.0F, 314.0F, 50.0F, 6.0F},
}};

constexpr std::array<Rect, 4> cargo{{
    {185.0F, 395.0F, 25.0F, 25.0F},
    {455.0F, 475.0F, 25.0F, 25.0F},
    {625.0F, 315.0F, 25.0F, 25.0F},
    {1505.0F, 235.0F, 25.0F, 25.0F},
}};

// Render camera only: simulation and network coordinates stay in world space.
constexpr float cameraTargetX(float playerX)
{
    return std::clamp(playerX + playerWidth * 0.5F - width * 0.5F,
                      0.0F, static_cast<float>(worldWidth - width));
}

constexpr std::uint8_t allCargoMask = (1U << cargo.size()) - 1U;

constexpr std::array<Network::SpawnPoint, 8> spawns{{
    {76.0F, 380.0F}, {112.0F, 380.0F}, {182.0F, 380.0F},
    {56.0F, 460.0F}, {290.0F, 460.0F}, {450.0F, 460.0F},
    {640.0F, 460.0F}, {810.0F, 460.0F},
}};

constexpr std::array<Network::PlatformPath, 2> shuttlePaths{{
    {1, 245.0F, 385.0F, 420.0F, 385.0F, 95.0F, 112.0F, 18.0F},
    {2, 640.0F, 295.0F, 750.0F, 295.0F, 80.0F, 100.0F, 18.0F},
}};

// The server also authors the patrol poses; the game interprets these IDs as hazards.
constexpr std::array<Network::PlatformPath, 3> dronePaths{{
    {11, 260.0F, 466.0F, 620.0F, 466.0F, 155.0F, 46.0F, 26.0F},
    {12, 645.0F, 226.0F, 790.0F, 226.0F, 125.0F, 46.0F, 26.0F},
    {13, 1020.0F, 466.0F, 1460.0F, 466.0F, 165.0F, 46.0F, 26.0F},
}};

constexpr bool isShuttle(std::uint32_t id) { return id == 1 || id == 2; }
constexpr bool isDrone(std::uint32_t id) { return id >= 11 && id <= 13; }

inline Network::ServerConfig serverConfig()
{
    Network::ServerConfig config;
    config.spawnPoints.assign(spawns.begin(), spawns.end());
    config.maxPlayers = 8;
    config.platforms.assign(shuttlePaths.begin(), shuttlePaths.end());
    config.platforms.insert(config.platforms.end(), dronePaths.begin(), dronePaths.end());
    return config;
}

} // namespace StarfallSalvage
