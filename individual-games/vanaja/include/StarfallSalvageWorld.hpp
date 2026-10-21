#pragma once

#include "Collision.hpp"
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
// Caps how far the astronaut can sink into a deck in one 20 ms frame (about
// 12 px), so the engine's collision pass lands it instead of pushing it
// sideways off an edge.
constexpr float terminalFallSpeed = 600.0F;

// The hull breach: a gap in the floor with a hidden death zone below it.
constexpr Rect hullBreach{520.0F, 500.0F, 80.0F, 40.0F};

// The floor makes most missed jumps recoverable; the shuttles bridge the climb.
// The second floor segment comes last so the other decks keep their indices.
constexpr std::array<Rect, 7> decks{{
    {0.0F, 500.0F, hullBreach.x, 40.0F},
    {50.0F, 420.0F, 180.0F, 24.0F},
    {510.0F, 340.0F, 150.0F, 24.0F},
    {815.0F, 260.0F, 125.0F, 24.0F},
    {1010.0F, 320.0F, 240.0F, 24.0F},
    {1290.0F, 260.0F, 260.0F, 24.0F},
    {hullBreach.x + hullBreach.width, 500.0F,
     static_cast<float>(worldWidth) - (hullBreach.x + hullBreach.width), 40.0F},
}};

// Hidden death zones: one just inside the breach, and a catch-all below the
// station for anything else that falls out of the world.
constexpr std::array<Rect, 2> deathZones{{
    {hullBreach.x, hullBreach.y + 15.0F, hullBreach.width, 160.0F},
    {-400.0F, static_cast<float>(height) + 60.0F, static_cast<float>(worldWidth) + 800.0F, 400.0F},
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

// None of these lies in a drone's or shuttle's path (checked by the tests), so
// nobody is hit or pushed the moment they spawn or respawn.
constexpr std::array<Network::SpawnPoint, 8> spawns{{
    {76.0F, 380.0F}, {112.0F, 380.0F}, {182.0F, 380.0F},
    {56.0F, 460.0F}, {700.0F, 460.0F}, {750.0F, 460.0F},
    {530.0F, 300.0F}, {810.0F, 460.0F},
}};

// Shuttle 1 runs sideways from the west floor to deck 2. Shuttle 2 is a
// vertical lift between deck 2 and deck 3.
constexpr std::array<Network::PlatformPath, 2> shuttlePaths{{
    {1, 245.0F, 385.0F, 420.0F, 385.0F, 95.0F, 112.0F, 18.0F},
    {2, 685.0F, 345.0F, 685.0F, 255.0F, 70.0F, 100.0F, 18.0F},
}};

// The server also authors the patrol poses; the game interprets these IDs as
// hazards. Drone 12 circles above deck 2: high enough to miss anyone standing
// on it or on relay 2, and left of anyone riding the lift, so only jumps there
// have to be timed.
constexpr std::array<Network::PlatformPath, 3> dronePaths{{
    {11, 260.0F, 466.0F, 620.0F, 466.0F, 155.0F, 46.0F, 26.0F},
    {12, 0.0F, 0.0F, 0.0F, 0.0F, 110.0F, 46.0F, 26.0F,
     Network::PlatformPath::Shape::Circular, 560.0F, 220.0F, 40.0F},
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
