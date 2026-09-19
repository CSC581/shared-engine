#pragma once

#include "NetworkServer.hpp"

#include <cstdint>

// Shared Apex tower coordinates for the headless server and the game client.
// Keep the moving-platform path here so online poses cannot drift from offline.
namespace ApexNetwork {

constexpr int designWidth = 900;
constexpr int designHeight = 900;
constexpr int roomCount = 6;

constexpr float groundThickness = 140.0F;
constexpr float platformThickness = 60.0F;
constexpr float playerWidth = 50.0F;
constexpr float playerHeight = 70.0F;

constexpr float movingPlatformWidth = 150.0F;
constexpr float movingPlatformSpeed = 150.0F;
constexpr float movingPointAX = 300.0F;
constexpr float movingPointBX = 550.0F;
// Offset from room-0 top (same as buildLevel in apexAscent.cpp).
constexpr float movingPlatformRoomOffsetY = 150.0F;
constexpr std::uint32_t movingPlatformId = 1;

inline float worldHeight()
{
    return static_cast<float>(designHeight * roomCount);
}

inline float room0Top()
{
    return worldHeight() - static_cast<float>(designHeight);
}

inline float movingPlatformY()
{
    return room0Top() + movingPlatformRoomOffsetY;
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
    config.platforms = {
        {movingPlatformId, movingPointAX, movingPlatformY(), movingPointBX, movingPlatformY(),
         movingPlatformSpeed, movingPlatformWidth, platformThickness},
    };
    return config;
}

} // namespace ApexNetwork
