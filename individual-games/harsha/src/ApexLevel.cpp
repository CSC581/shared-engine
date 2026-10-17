#include "ApexLevel.hpp"

namespace ApexLevel {

Color colorFor(ApexPlatform::Kind kind)
{
    switch (kind) {
    case ApexPlatform::Kind::Mover:
        // Amber so moving platforms read as intentional.
        return {200, 150, 60};
    case ApexPlatform::Kind::Moss:
        return {90, 150, 80};
    case ApexPlatform::Kind::Sandstone:
        return {200, 170, 110};
    case ApexPlatform::Kind::Ice:
        // Pale cyan: reads as slippery before you touch it.
        return {170, 220, 235};
    case ApexPlatform::Kind::Slingshot:
        return {220, 60, 200};
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

// Room 1, the lift. The moss ledges sit too high above the sandstone to jump
// to (a jump that rises h also travels about 1.1h sideways, and the room is
// too narrow), so the elevator (movers(), id 2) is the only way up.
// Heights were tuned with a jump simulation so every jump allows at least
// ~0.14 s of charge error.
void buildRoom1(const Room& room)
{
    constexpr float mossWidth = 90.0F;

    // Landing from room 0's mover; wide and forgiving.
    makeStatic(room.world, 520.0F, room.top + 800.0F, 340.0F, platformThickness, Kind::Sandstone);

    // Off the top of the lift, then up to the ledge below room 2.
    makeStatic(room.world, 380.0F, room.top + 260.0F, mossWidth, platformThickness, Kind::Moss);
    makeStatic(room.world, 680.0F, room.top + 120.0F, mossWidth, platformThickness, Kind::Moss);
}

// Room 2, ice. Three ice ledges zig-zag up; the last (top left) is the one
// you have to land on, and a stone shelf waits under it to catch a short jump
// or a slide off the edge. The shelf becomes a spike death zone (Step 4).
void buildRoom2(const Room& room)
{
    makeStatic(room.world, 260.0F, room.top + 760.0F, 260.0F, platformThickness, Kind::Ice);
    makeStatic(room.world, 560.0F, room.top + 480.0F, 260.0F, platformThickness, Kind::Ice);
    makeStatic(room.world, 60.0F, room.top + 180.0F, 220.0F, platformThickness, Kind::Ice);

    // Spike shelf under the top ledge, wide enough to catch a slide off
    // either edge.
    makeStatic(room.world, 20.0F, room.top + 500.0F, 340.0F, platformThickness, Kind::Stone);
}

// Room 3, the slingshot. A fast magenta rail (movers(), id 3) shuttles over a
// pit; ride it from the launch ledge and jump off at its right end to reach
// the goal ledge. The pit floor becomes a spike death zone (Step 4).
void buildRoom3(const Room& room)
{
    // Launch ledge, reached from room 2's top ice ledge.
    makeStatic(room.world, 380.0F, room.top + 840.0F, 180.0F, platformThickness, Kind::Stone);

    // Goal ledge, up and right of the rail's far end.
    makeStatic(room.world, 740.0F, room.top + 240.0F, 140.0F, platformThickness, Kind::Stone);

    // Pit floor under the rail's far end and the goal; climbable back to the
    // launch ledge until it gets spikes.
    makeStatic(room.world, 620.0F, room.top + 900.0F, 280.0F, platformThickness, Kind::Stone);
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
    buildRoom1(room(1));
    buildRoom2(room(2));
    buildRoom3(room(3));
    for (int index = 4; index < ApexNetwork::roomCount; ++index) {
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
