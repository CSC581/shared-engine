#include "ApexLevel.hpp"

#include "ApexNetworkConfig.hpp"

namespace ApexLevel {

Color colorFor(ApexPlatform::Kind kind)
{
    switch (kind) {
    case ApexPlatform::Kind::Mover:
        // Amber so moving platforms read as intentional.
        return {200, 150, 60};
    case ApexPlatform::Kind::Stone:
    default:
        return {90, 100, 130};
    }
}

GameObject& makeStatic(World& world, float x, float y, float width, float height,
                       ApexPlatform::Kind kind)
{
    GameObject& platform = world.create("platform");
    platform.add<Transform>(x, y, width, height);
    platform.add<Collider>(Collider::Kind::Solid, platformLayer);
    platform.add<Renderable>(colorFor(kind));
    platform.add<ApexPlatform>(kind);
    platform.add<Surface>();
    platform.setActive(true);
    return platform;
}

GameObject& makeMover(World& world, PathMover::Linear path, float speed, float width,
                      float height)
{
    constexpr auto kind = ApexPlatform::Kind::Mover;

    GameObject& platform = world.create("platform");
    platform.add<Transform>(path.startX, path.startY, width, height);
    platform.add<Collider>(Collider::Kind::Solid, platformLayer);
    platform.add<Renderable>(colorFor(kind));
    platform.add<ApexPlatform>(kind);
    platform.add<Surface>();
    // PathMover pulls in Motion, makes it kinematic and snaps onto the path
    // start, so Carrier (added after) measures from the right place.
    platform.add<PathMover>(path, speed);
    platform.add<Carrier>();
    platform.setActive(true);
    return platform;
}

void buildLevel(World& world, float worldWidth, float worldHeight, float viewHeight, bool online)
{
    using Kind = ApexPlatform::Kind;

    constexpr float groundThickness = ApexNetwork::groundThickness;
    constexpr float wallThickness = 40.0F;
    constexpr float platformThickness = ApexNetwork::platformThickness;

    // Ground at the bottom of the world.
    makeStatic(world, 0.0F, worldHeight - groundThickness, worldWidth, groundThickness, Kind::Stone);

    // Side walls for the whole climb.
    makeStatic(world, -wallThickness, 0.0F, wallThickness, worldHeight, Kind::Stone);
    makeStatic(world, worldWidth, 0.0F, wallThickness, worldHeight, Kind::Stone);

    // Hand-placed first room (easy jumps to learn charging).
    const float room0Top = worldHeight - viewHeight;
    makeStatic(world, 160.0F, room0Top + 620.0F, 220.0F, platformThickness, Kind::Stone);
    makeStatic(world, 560.0F, room0Top + 460.0F, 220.0F, platformThickness, Kind::Stone);
    makeStatic(world, 220.0F, room0Top + 280.0F, 220.0F, platformThickness, Kind::Stone);

    // Auto-moving platform (matches ApexNetwork::makeServerConfig for online).
    if (!online) {
        const float y = ApexNetwork::movingPlatformY();
        makeMover(world, {ApexNetwork::movingPointAX, y, ApexNetwork::movingPointBX, y},
                  ApexNetwork::movingPlatformSpeed, ApexNetwork::movingPlatformWidth,
                  platformThickness);
    }

    // TODO: replace generated staircase with hand-designed rooms later.
    for (int room = 1; room < ApexNetwork::roomCount; ++room) {
        const float roomTop = worldHeight - viewHeight * static_cast<float>(room + 1);
        const bool startLeft = room % 2 == 0;

        const float leftX = 120.0F;
        const float rightX = worldWidth - 120.0F - 220.0F;

        makeStatic(world, startLeft ? leftX : rightX, roomTop + 700.0F, 220.0F, platformThickness,
                   Kind::Stone);
        makeStatic(world, startLeft ? rightX : leftX, roomTop + 460.0F, 220.0F, platformThickness,
                   Kind::Stone);
        makeStatic(world, worldWidth * 0.5F - 110.0F, roomTop + 220.0F, 220.0F, platformThickness,
                   Kind::Stone);
    }
}

} // namespace ApexLevel
