#include "ApexLevel.hpp"

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

ApexPlatform::Kind kindFor(ApexNetwork::MoverStyle style)
{
    switch (style) {
    case ApexNetwork::MoverStyle::Slingshot:
        return ApexPlatform::Kind::Slingshot;
    case ApexNetwork::MoverStyle::Orbit:
        return ApexPlatform::Kind::Orbit;
    case ApexNetwork::MoverStyle::Plain:
    default:
        return ApexPlatform::Kind::Mover;
    }
}

GameObject& makeMover(World& world, const ApexNetwork::MoverSpec& spec)
{
    const Network::PlatformPath& path = spec.path;
    const ApexPlatform::Kind kind = kindFor(spec.style);

    GameObject& platform = world.create("platform");
    platform.add<Transform>(0.0F, 0.0F, path.width, path.height);
    platform.add<Collider>(Collider::Kind::Solid, platformLayer);
    platform.add<Renderable>(colorFor(kind));
    platform.add<ApexPlatform>(kind);
    platform.add<Surface>();
    // PathMover pulls in Motion, makes it kinematic and snaps onto the path
    // start, so Carrier (added after) measures from the right place. Same
    // construction as NetworkServer, so offline and online paths match.
    if (path.shape == Network::PlatformPath::Shape::Circular) {
        platform.add<PathMover>(PathMover::Circular{path.centerX, path.centerY, path.radius}, path.speed);
    } else {
        platform.add<PathMover>(PathMover::Linear{path.startX, path.startY, path.endX, path.endY},
                                path.speed);
    }
    platform.add<Carrier>();
    platform.setActive(true);
    return platform;
}

GameObject& makePlayer(World& world, float x, float y, float width, float height, float gravity)
{
    GameObject& player = world.create("player");
    player.add<Transform>(x, y, width, height);
    player.add<Motion>();
    player.add<Gravity>(gravity);
    player.add<Collider>(Collider::Kind::Solid, platformLayer | triggerLayer);
    player.setActive(true);
    return player;
}

GameObject& makeRemotePlayer(World& world, float width, float height)
{
    GameObject& remote = world.create("remote");
    remote.add<Transform>(0.0F, 0.0F, width, height);
    return remote;
}

GameObject& makeServerPlatform(World& world, std::uint32_t id)
{
    // Kind from the shared mover table; an id the table does not know is
    // still a plain mover.
    const ApexNetwork::MoverSpec* spec = ApexNetwork::findMover(id);
    const ApexPlatform::Kind kind = spec != nullptr ? kindFor(spec->style) : ApexPlatform::Kind::Mover;

    GameObject& platform = world.create("platform");
    platform.add<Transform>();
    platform.add<Collider>(Collider::Kind::Solid, platformLayer);
    platform.add<Renderable>(colorFor(kind));
    platform.add<ApexPlatform>(kind);
    platform.add<Surface>();
    // Measures each snapshot's move, so carry works exactly as offline.
    platform.add<Carrier>();
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

    // Moving platforms, from the table the server also runs.
    if (!online) {
        for (const ApexNetwork::MoverSpec& spec : ApexNetwork::movers()) {
            makeMover(world, spec);
        }
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
