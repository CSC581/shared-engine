#include "StarfallSalvageLevel.hpp"

#include "StarfallSalvageWorld.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace StarfallSalvage {
namespace {

constexpr Color cargoColor{213, 112, 47};
constexpr Color relayIdleColor{70, 104, 134};
constexpr Color gatePlateColor{247, 180, 93};
constexpr Color shuttleColor{44, 70, 100};

// A shuttle may jump a little between snapshots, but a move this large means
// the astronaut was not really riding it.
constexpr float maximumCarry = 32.0F;

} // namespace

DeckPalette paletteFor(DeckKind kind)
{
    switch (kind) {
    case DeckKind::Hull:
        return {{37, 58, 86}, {115, 174, 208}, {65, 100, 137}, {80, 118, 151}};
    case DeckKind::West:
        return {{48, 78, 118}, {140, 198, 232}, {82, 122, 164}, {100, 142, 182}};
    case DeckKind::East:
        return {{36, 84, 80}, {108, 214, 186}, {60, 124, 116}, {80, 150, 138}};
    }
    return {{37, 58, 86}, {115, 174, 208}, {65, 100, 137}, {80, 118, 151}};
}

DeckKind deckKindFor(std::size_t deckIndex)
{
    // decks[0] and decks[6] are the two floor segments either side of the
    // breach; decks[4] and decks[5] are the east wing beyond the bulkhead.
    if (deckIndex == 0 || deckIndex == 6) {
        return DeckKind::Hull;
    }
    return deckIndex >= 4 ? DeckKind::East : DeckKind::West;
}

// --- Astronaut ---------------------------------------------------------------

void Astronaut::onAttach()
{
    transform_ = &owner().require<Transform>();
    motion_ = &owner().require<Motion>();
    Collider& collider = owner().require<Collider>();
    collider.onCollide = [this](GameObject&, GameObject& other) { touch(other); };
    collider.onEnter = [this](GameObject&, GameObject& other) {
        if (other.tag() == "deathzone") {
            respawn();
        }
    };
}

void Astronaut::update(const FrameTime& time)
{
    const auto dt = static_cast<float>(time.dtSeconds);
    if (dt <= 0.0F) {
        return;
    }

    invulnerableSeconds = std::max(0.0F, invulnerableSeconds - dt);
    motion_->velocityX = input.axis * playerSpeed;
    if (input.jump && jumpsUsed < maximumJumps) {
        motion_->velocityY = -jumpSpeed;
        ++jumpsUsed;
        grounded = false;
        supportId = 0;
    }
    motion_->velocityY = std::min(motion_->velocityY, terminalFallSpeed);
}

void Astronaut::beginCollisionPass()
{
    grounded = false;
    supportId = 0;
}

void Astronaut::rideShuttle(const GameObject* shuttle)
{
    if (supportId == 0) {
        return;
    }
    if (shuttle == nullptr) {
        detachFromShuttle();
        return;
    }

    const Transform& pose = *shuttle->get<Transform>();
    const float dx = pose.x - supportX_;
    const float dy = pose.y - supportY_;
    if (std::fabs(dx) > maximumCarry || std::fabs(dy) > maximumCarry) {
        detachFromShuttle();
        return;
    }
    transform_->x += dx;
    transform_->y += dy;
    supportX_ = pose.x;
    supportY_ = pose.y;
}

void Astronaut::detachFromShuttle()
{
    supportId = 0;
    grounded = false;
}

void Astronaut::respawn()
{
    transform_->x = spawn.x;
    transform_->y = spawn.y;
    motion_->velocityX = 0.0F;
    motion_->velocityY = 0.0F;
    grounded = false;
    jumpsUsed = 0;
    supportId = 0;
}

void Astronaut::touch(GameObject& other)
{
    const std::string& tag = other.tag();
    if (tag == "drone") {
        hitDrone();
        return;
    }
    if (tag != "deck" && tag != "shuttle" && tag != "bulkhead") {
        return;
    }

    // The collision pass has already pushed the astronaut out. It is standing
    // on `other` when that push left its feet exactly on the top surface.
    const Rect self = transform_->bounds();
    const Rect surface = other.get<Transform>()->bounds();
    const bool onTop = std::fabs(self.y + self.height - surface.y) <= 0.5F &&
                       self.x < surface.x + surface.width && self.x + self.width > surface.x &&
                       motion_->velocityY >= 0.0F;
    if (!onTop) {
        return;
    }

    grounded = true;
    jumpsUsed = 0;
    if (tag == "shuttle") {
        if (const auto* identity = other.get<NetworkIdentity>()) {
            supportId = identity->id;
            supportX_ = surface.x;
            supportY_ = surface.y;
        }
    }
}

void Astronaut::hitDrone()
{
    if (invulnerableSeconds > 0.0F || lives <= 0) {
        return;
    }
    --lives;
    if (lives > 0) {
        respawn();
        invulnerableSeconds = 1.0F;
    } else {
        motion_->velocityX = 0.0F;
        motion_->velocityY = 0.0F;
        grounded = false;
        supportId = 0;
    }
}

// --- Factories ---------------------------------------------------------------

GameObject& makeDeck(World& world, const Rect& box, DeckKind kind)
{
    GameObject& deck = world.create("deck");
    deck.add<Transform>(box.x, box.y, box.width, box.height);
    deck.add<Collider>(Collider::Kind::Solid, solidLayer);
    deck.add<Renderable>(paletteFor(kind).base).layer = deckDrawLayer;
    deck.add<DeckStyle>(kind);
    deck.setActive(true);
    return deck;
}

GameObject& makeSpawnPoint(World& world, float x, float y)
{
    // No Renderable: spawn points are never drawn.
    GameObject& spawn = world.create("spawn");
    spawn.add<Transform>(x, y, playerWidth, playerHeight);
    spawn.setActive(true);
    return spawn;
}

GameObject& makeDeathZone(World& world, const Rect& box)
{
    // A hidden trigger: no Renderable, and it never pushes anything.
    GameObject& zone = world.create("deathzone");
    zone.add<Transform>(box.x, box.y, box.width, box.height);
    zone.add<Collider>(Collider::Kind::Trigger, zoneLayer);
    zone.setActive(true);
    return zone;
}

GameObject& makeFixture(World& world, const char* tag, const Rect& box, Color color)
{
    GameObject& fixture = world.create(tag);
    fixture.add<Transform>(box.x, box.y, box.width, box.height);
    fixture.add<Renderable>(color).layer = fixtureDrawLayer;
    fixture.setActive(true);
    return fixture;
}

GameObject& makeBulkhead(World& world)
{
    // Drawn by the game, which shows it open or closed. Opening it clears the
    // collider's layers rather than removing the component.
    GameObject& gate = world.create("bulkhead");
    gate.add<Transform>(bulkhead.x, bulkhead.y, bulkhead.width, bulkhead.height);
    gate.add<Collider>(Collider::Kind::Solid, solidLayer);
    gate.setActive(true);
    return gate;
}

GameObject& makePlayer(World& world)
{
    // Drawn by the game as a multi-part sprite, so it has no Renderable fill.
    GameObject& player = world.create("player");
    player.add<Transform>(spawns[0].x, spawns[0].y, playerWidth, playerHeight);
    player.add<Motion>();
    player.add<Gravity>(gravity);
    player.add<Collider>(Collider::Kind::Solid, solidLayer | hazardLayer | zoneLayer);
    Astronaut& astronaut = player.add<Astronaut>();
    astronaut.spawn = {spawns[0].x, spawns[0].y, playerWidth, playerHeight};
    player.setActive(true);
    return player;
}

GameObject& makeRemotePlayer(World& world, const Multiplayer::Player&)
{
    // NetworkWorldSync positions and activates it. No Motion: the network owns
    // its position. No Collider: astronauts pass through each other.
    GameObject& remote = world.create("remote");
    remote.add<Transform>(0.0F, 0.0F, playerWidth, playerHeight);
    return remote;
}

GameObject& makeNetworkPlatform(World& world, const Multiplayer::Platform& platform)
{
    // Size and position come from the snapshot through NetworkWorldSync.
    if (isShuttle(platform.id)) {
        GameObject& shuttle = world.create("shuttle");
        shuttle.add<Transform>();
        shuttle.add<Collider>(Collider::Kind::Solid, solidLayer);
        shuttle.add<Renderable>(shuttleColor).layer = deckDrawLayer;
        return shuttle;
    }
    if (isDrone(platform.id)) {
        GameObject& drone = world.create("drone");
        drone.add<Transform>();
        drone.add<Collider>(Collider::Kind::Trigger, hazardLayer);
        return drone;
    }
    GameObject& unknown = world.create("platform");
    unknown.add<Transform>();
    return unknown;
}

Station buildStation(World& world)
{
    Station station;
    for (std::size_t i = 0; i < decks.size(); ++i) {
        makeDeck(world, decks[i], deckKindFor(i));
    }
    for (const Network::SpawnPoint& spawn : spawns) {
        station.spawns.push_back(&makeSpawnPoint(world, spawn.x, spawn.y));
    }
    for (const Rect& zone : deathZones) {
        makeDeathZone(world, zone);
    }
    for (const Rect& crate : cargo) {
        station.cargo.push_back(&makeFixture(world, "cargo", crate, cargoColor));
    }
    for (const Rect& pad : relayPads) {
        station.relayPads.push_back(&makeFixture(world, "relay", pad, relayIdleColor));
    }
    for (const Rect& plate : gatePlates) {
        station.gatePlates.push_back(&makeFixture(world, "plate", plate, gatePlateColor));
    }
    station.bulkhead = &makeBulkhead(world);
    return station;
}

} // namespace StarfallSalvage
