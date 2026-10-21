#include "ApexLevel.hpp"

#include <vector>

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
    case ApexPlatform::Kind::Orbit:
        return {150, 110, 230};
    case ApexPlatform::Kind::Summit:
        return {240, 200, 70};
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
    platform.add<Surface>(kind == ApexPlatform::Kind::Ice ? Surface::ice : Surface::ground);
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

GameObject& makeSpawnMarker(World& world, float x, float y, float width, int room, int slot)
{
    GameObject& marker = world.create("trigger");
    marker.add<Transform>(x, y, width, ApexNetwork::playerHeight);
    marker.add<Collider>(Collider::Kind::Trigger, triggerLayer);
    marker.add<SpawnMarker>(room, slot);
    marker.setActive(true);
    return marker;
}

GameObject& makeDeathZone(World& world, float x, float y, float width, float height, bool spikes)
{
    GameObject& zone = world.create("trigger");
    zone.add<Transform>(x, y, width, height);
    zone.add<Collider>(Collider::Kind::Trigger, triggerLayer);
    zone.add<DeathZone>(spikes);
    zone.setActive(true);
    return zone;
}

GameObject& makeRoomZone(World& world, int room, float top, float width, float height)
{
    GameObject& zone = world.create("trigger");
    zone.add<Transform>(0.0F, top, width, height);
    zone.add<Collider>(Collider::Kind::Trigger, triggerLayer);
    zone.add<RoomZone>(room);
    zone.setActive(true);
    return zone;
}

GameObject& makeWindZone(World& world, float x, float y, float width, float height, float amplitude,
                         float period)
{
    GameObject& zone = world.create("trigger");
    zone.add<Transform>(x, y, width, height);
    zone.add<Collider>(Collider::Kind::Trigger, triggerLayer);
    zone.add<WindZone>(amplitude, period);
    zone.setActive(true);
    return zone;
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

// Room 5 wind: peak sideways push (u/s^2) and the time for one left-right
// swing. Tuned in the real game so the narrow ledges stay reachable.
constexpr float windAmplitude = 300.0F;
constexpr float windPeriod = 4.0F;
constexpr float platformThickness = ApexNetwork::platformThickness;

// One room's frame of reference: y offsets inside a room are measured from
// its top, so a room can be laid out (and moved) on its own.
struct Room {
    World& world;
    float top;
    float width;
};

// The checkpoint for a room: a spawn marker covering the top of its first
// ledge, so landing anywhere on it counts.
void checkpointOn(const Room& room, int index, float ledgeX, float ledgeTop, float ledgeWidth)
{
    makeSpawnMarker(room.world, ledgeX, ledgeTop - ApexNetwork::playerHeight, ledgeWidth, index, 0);
}

// Room 0, the tutorial: three stone ledges that teach a short, a medium and a
// sideways charge. The horizontal mover above them (movers(), id 1) teaches
// timing.
void buildRoom0(const Room& room)
{
    makeStatic(room.world, 160.0F, room.top + 620.0F, 220.0F, platformThickness, Kind::Stone);
    makeStatic(room.world, 560.0F, room.top + 460.0F, 220.0F, platformThickness, Kind::Stone);
    makeStatic(room.world, 220.0F, room.top + 280.0F, 220.0F, platformThickness, Kind::Stone);

    // The four starting slots on the ground.
    const std::vector<Network::SpawnPoint> slots = ApexNetwork::spawnSlots();
    for (int slot = 0; slot < static_cast<int>(slots.size()); ++slot) {
        makeSpawnMarker(room.world, slots[slot].x, slots[slot].y, ApexNetwork::playerWidth, 0, slot);
    }
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
    checkpointOn(room, 1, 520.0F, room.top + 800.0F, 340.0F);

    // Off the top of the lift, then up to the ledge below room 2. The lower
    // ledge sits at x 460 (was 380) so the jump between them stays in this
    // room even with A/D held in mid-air (design §11.29).
    makeStatic(room.world, 460.0F, room.top + 260.0F, mossWidth, platformThickness, Kind::Moss);
    makeStatic(room.world, 680.0F, room.top + 120.0F, mossWidth, platformThickness, Kind::Moss);
}

// Room 2, ice. Three ice ledges zig-zag up; the last (top left) is the one
// you have to land on, and a stone shelf waits under it to catch a short jump
// or a slide off the edge. The shelf becomes a spike death zone (Step 4).
void buildRoom2(const Room& room)
{
    makeStatic(room.world, 260.0F, room.top + 760.0F, 260.0F, platformThickness, Kind::Ice);
    checkpointOn(room, 2, 260.0F, room.top + 760.0F, 260.0F);
    makeStatic(room.world, 560.0F, room.top + 480.0F, 260.0F, platformThickness, Kind::Ice);
    makeStatic(room.world, 60.0F, room.top + 180.0F, 220.0F, platformThickness, Kind::Ice);

    // Spike shelf under the top ledge: it catches a short jump at ice C and a
    // slow slide off either edge (a full-speed walk off the right edge sails
    // past it, down to ice A). The death zone is a strip on its top.
    makeStatic(room.world, 20.0F, room.top + 500.0F, 340.0F, platformThickness, Kind::Stone);
    makeDeathZone(room.world, 20.0F, room.top + 470.0F, 340.0F, 30.0F, true);
}

// Room 3, the slingshot. The fast magenta rail (movers(), id 3) runs low in
// the room; the only way on is the goal ledge high above and ~350 px to the
// left. An ordinary jump that high is forced to fly ~800 px sideways (sideways
// speed is tied to power), so it cannot land there. Jumping straight up (no
// aim) off the rail while it moves left borrows the rail's speed instead: a
// steep throw that lands on the ledge. Verified by simulation with wall and
// ceiling collisions: reachable only with the fling, at near-full charge.
void buildRoom3(const Room& room)
{
    makeStatic(room.world, 110.0F, room.top + 200.0F, 170.0F, platformThickness, Kind::Stone);
    checkpointOn(room, 3, 110.0F, room.top + 200.0F, 170.0F);
}

// Room 4, the orbit. A stone hub, then three violet wheels (movers(), ids
// 4-6) circling their own centres, each higher and slower than the last. The
// challenge is landing on a moving target; the 3:2:1 speeds make the pattern
// repeat every 8 s, so it can be learned.
void buildRoom4(const Room& room)
{
    makeStatic(room.world, 300.0F, room.top + 880.0F, 180.0F, platformThickness, Kind::Stone);
    checkpointOn(room, 4, 300.0F, room.top + 880.0F, 180.0F);
}

// Room 5, the summit. Narrow stone ledges zig-zag up to the gold summit at
// the top of the tower. Step 3 adds wind here, which is why the ledges come
// last: they are the precision test.
void buildRoom5(const Room& room)
{
    constexpr float ledgeWidth = 110.0F;

    // First ledge far left, clear of wheel 6's path: a ledge above the wheel
    // can only be hit from underneath.
    makeStatic(room.world, 100.0F, room.top + 780.0F, 160.0F, platformThickness, Kind::Stone);
    checkpointOn(room, 5, 100.0F, room.top + 780.0F, 160.0F);
    makeStatic(room.world, 560.0F, room.top + 520.0F, ledgeWidth, platformThickness, Kind::Stone);
    makeStatic(room.world, 140.0F, room.top + 290.0F, ledgeWidth, platformThickness, Kind::Stone);

    makeStatic(room.world, 560.0F, room.top + 100.0F, 260.0F, platformThickness, Kind::Summit);

    // The whole room is gusty. Hidden: the game draws streaks as the cue.
    makeWindZone(room.world, 0.0F, room.top, room.width, 900.0F, windAmplitude, windPeriod);
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
    // Hidden room boundaries for the room-snap camera, one per room.
    for (int index = 0; index < ApexNetwork::roomCount; ++index) {
        makeRoomZone(world, index, room(index).top, worldWidth, viewHeight);
    }

    buildRoom0(room(0));
    buildRoom1(room(1));
    buildRoom2(room(2));
    buildRoom3(room(3));
    buildRoom4(room(4));
    buildRoom5(room(5));

    // Failsafe under the floor: if anything ever drops a climber out of the
    // world, it comes back instead of falling forever.
    makeDeathZone(world, -wallThickness, worldHeight, worldWidth + 2.0F * wallThickness, 400.0F, false);

    // Moving platforms, from the table the server also runs.
    if (!online) {
        for (const ApexNetwork::MoverSpec& spec : ApexNetwork::movers()) {
            makeMover(world, spec);
        }
    }
}

} // namespace ApexLevel
