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

// Top edge of room `index` (0 = bottom room) in world coordinates.
inline float roomTop(int index)
{
    return worldHeight() - static_cast<float>(designHeight * (index + 1));
}

inline float room0Top()
{
    return roomTop(0);
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

        // Room 1: elevator on the left wall, from the sandstone ledge's height
        // up to the moss route.
        Network::PlatformPath elevator;
        elevator.id = 2;
        elevator.startX = 100.0F;
        elevator.startY = roomTop(1) + 700.0F;
        elevator.endX = elevator.startX;
        elevator.endY = roomTop(1) + 300.0F;
        elevator.speed = 120.0F;
        elevator.width = 130.0F;
        elevator.height = platformThickness;
        specs.push_back({MoverStyle::Plain, elevator});

        // Room 3: fast slingshot rail low in the room. The goal ledge is high
        // above and only a short way left; only a fling gets there (see
        // ApexLevel buildRoom3).
        Network::PlatformPath rail;
        rail.id = 3;
        rail.startX = 420.0F;
        rail.startY = roomTop(3) + 880.0F;
        rail.endX = 640.0F;
        rail.endY = rail.startY;
        rail.speed = 420.0F;
        rail.width = 140.0F;
        rail.height = platformThickness;
        specs.push_back({MoverStyle::Slingshot, rail});

        // Room 4: three orbit wheels stacked up the room. Angular speeds are
        // 3:2:1, so every wheel is back at its start (angle 0) together every
        // orbitPeriod seconds and the whole pattern repeats exactly.
        // PathMover::Circular takes a rim speed, so speed = radius * omega.
        constexpr float orbitPeriod = 8.0F;
        constexpr float baseOmega = 2.0F * 3.14159265F / orbitPeriod;
        const auto orbit = [](std::uint32_t id, float centerX, float centerY, float radius,
                              float omega) {
            Network::PlatformPath path;
            path.id = id;
            path.shape = Network::PlatformPath::Shape::Circular;
            path.centerX = centerX;
            path.centerY = centerY;
            path.radius = radius;
            path.speed = radius * omega;
            path.width = 110.0F;
            path.height = platformThickness;
            return MoverSpec{MoverStyle::Orbit, path};
        };
        specs.push_back(orbit(4, 600.0F, roomTop(4) + 640.0F, 100.0F, 3.0F * baseOmega));
        specs.push_back(orbit(5, 200.0F, roomTop(4) + 380.0F, 120.0F, 2.0F * baseOmega));
        specs.push_back(orbit(6, 580.0F, roomTop(4) + 110.0F, 140.0F, 1.0F * baseOmega));

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
