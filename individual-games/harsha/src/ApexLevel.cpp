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

namespace {

using Kind = ApexPlatform::Kind;

constexpr float wallThickness = 40.0F;
constexpr float platformThickness = ApexNetwork::platformThickness;

// One room's frame of reference: y offsets inside a room are measured from
// its top, so a room can be laid out (and moved) on its own.
struct Room {
    World& world;
    float top;
    float width;
};

// Room 0, the tutorial: three stone ledges that teach a short, a medium and a
// sideways charge. The horizontal mover above them (movers(), id 1) teaches
// timing.
void buildRoom0(const Room& room)
{
    makeStatic(room.world, 160.0F, room.top + 620.0F, 220.0F, platformThickness, Kind::Stone);
    makeStatic(room.world, 560.0F, room.top + 460.0F, 220.0F, platformThickness, Kind::Stone);
    makeStatic(room.world, 220.0F, room.top + 280.0F, 220.0F, platformThickness, Kind::Stone);
}

// Placeholder zig-zag for rooms not designed yet; each is replaced by its own
// buildRoomN as the rooms land.
void buildStaircaseRoom(const Room& room, int index)
{
    const bool startLeft = index % 2 == 0;
    const float leftX = 120.0F;
    const float rightX = room.width - 120.0F - 220.0F;

    makeStatic(room.world, startLeft ? leftX : rightX, room.top + 700.0F, 220.0F, platformThickness,
               Kind::Stone);
    makeStatic(room.world, startLeft ? rightX : leftX, room.top + 460.0F, 220.0F, platformThickness,
               Kind::Stone);
    makeStatic(room.world, room.width * 0.5F - 110.0F, room.top + 220.0F, 220.0F, platformThickness,
               Kind::Stone);
}

} // namespace

void buildLevel(World& world, float worldWidth, float worldHeight, float viewHeight, bool online)
{
    constexpr float groundThickness = ApexNetwork::groundThickness;

    // Ground at the bottom of the world.
    makeStatic(world, 0.0F, worldHeight - groundThickness, worldWidth, groundThickness, Kind::Stone);

    // Side walls for the whole climb.
    makeStatic(world, -wallThickness, 0.0F, wallThickness, worldHeight, Kind::Stone);
    makeStatic(world, worldWidth, 0.0F, wallThickness, worldHeight, Kind::Stone);

    // Rooms bottom to top; room N's top is N+1 screens above the world floor.
    const auto room = [&](int index) {
        return Room{world, worldHeight - viewHeight * static_cast<float>(index + 1), worldWidth};
    };
    buildRoom0(room(0));
    for (int index = 1; index < ApexNetwork::roomCount; ++index) {
        buildStaircaseRoom(room(index), index);
    }

    // Moving platforms, from the table the server also runs.
    if (!online) {
        for (const ApexNetwork::MoverSpec& spec : ApexNetwork::movers()) {
            makeMover(world, spec);
        }
    }
}

} // namespace ApexLevel
