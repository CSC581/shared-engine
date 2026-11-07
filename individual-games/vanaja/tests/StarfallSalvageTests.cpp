#include "StarfallSalvageEvents.hpp"
#include "StarfallSalvageLevel.hpp"
#include "StarfallSalvageRules.hpp"
#include "StarfallSalvageWorld.hpp"
#include "Components.hpp"
#include "DeltaTimer.hpp"
#include "EventManager.hpp"
#include "SendPacer.hpp"
#include "Timeline.hpp"
#include "World.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { \
        std::cerr << "FAILED: " #condition " at line " << __LINE__ << '\n'; \
        return 1; \
    } \
} while (false)

namespace {

class TestClock final : public TimeSource {
public:
    std::int64_t now() const override { return elapsed; }
    std::int64_t elapsed = 0;
};

int checkTimeAndPacing()
{
    constexpr std::int64_t heartbeat = 250 * kNsPerMs;
    for (const double scale : {0.5, 1.0, 2.0}) {
        TestClock real;
        Timeline gameTime(real, kNsPerUs);
        Timeline networkTime(real, kNsPerUs);
        gameTime.setScale(scale);
        networkTime.setScale(scale);
        DeltaTimer timer(gameTime, 50'000);
        Multiplayer::SendPacer pacer(&networkTime,
            StarfallSalvage::positionSendIntervalGameTics, real, heartbeat);
        const auto interval = StarfallSalvage::frameIntervalNs(scale);
        int sends = 0;
        std::int64_t totalGameTics = 0;
        EXPECT(pacer.shouldSend()); // initial publication is immediate
        EXPECT(!pacer.shouldSend());

        for (std::int64_t elapsed = 0; elapsed < 2 * kNsPerSec; elapsed += interval) {
            real.elapsed += interval;
            const auto delta = timer.tick();
            // Changing both loop frequency and time scale keeps each step at
            // 20 ms of game time, below the engine's 50 ms clamp.
            EXPECT(delta == StarfallSalvage::positionSendIntervalGameTics);
            EXPECT(!timer.lastWasClamped());
            totalGameTics += delta;
            if (pacer.shouldSend()) ++sends;
            EXPECT(!pacer.shouldSend());
        }
        EXPECT(sends == static_cast<int>(2 * StarfallSalvage::normalLoopHz * scale));
        EXPECT(totalGameTics == static_cast<std::int64_t>(2 * kGameTicsPerSecond * scale));

        gameTime.pause();
        const auto pausedAt = gameTime.now();
        const auto networkAtPause = networkTime.now();
        int pausedSends = 0;
        for (std::int64_t elapsed = 0; elapsed < 2 * kNsPerSec; elapsed += interval) {
            real.elapsed += interval;
            EXPECT(timer.tick() == 0);
            EXPECT(gameTime.now() == pausedAt);
            EXPECT(pacer.shouldSend());
            ++pausedSends;
            EXPECT(!pacer.shouldSend());
        }
        EXPECT(pausedSends == static_cast<int>(2 * StarfallSalvage::normalLoopHz * scale));
        EXPECT(networkTime.now() - networkAtPause ==
               static_cast<std::int64_t>(2 * kGameTicsPerSecond * scale));

        // Speed changes keep the send clock in sync without unpausing physics.
        const double newScale = scale == 2.0 ? 0.5 : 2.0;
        gameTime.setScale(newScale);
        networkTime.setScale(newScale);
        EXPECT(gameTime.now() == pausedAt);
        gameTime.unpause();
        real.elapsed += StarfallSalvage::frameIntervalNs(newScale);
        EXPECT(timer.tick() == StarfallSalvage::positionSendIntervalGameTics);
        EXPECT(pacer.shouldSend());
        EXPECT(!pacer.shouldSend());
        real.elapsed += 5 * kNsPerSec;
        EXPECT(timer.tick() == 50'000);
        EXPECT(timer.lastWasClamped());
        EXPECT(pacer.shouldSend());
        EXPECT(!pacer.shouldSend());
    }
    return 0;
}

StarfallSalvage::Claim onPad(std::uint32_t id, int pad, std::uint8_t cargoMask)
{
    const Rect area = StarfallSalvage::relayPads[static_cast<std::size_t>(pad)];
    return {id, area.x + 10.0F,
            area.y + area.height - StarfallSalvage::playerHeight,
            StarfallSalvage::encodeState(cargoMask, pad, cargoMask)};
}

StarfallSalvage::Claim onGate(std::uint32_t id, int plate)
{
    const Rect area = StarfallSalvage::gatePlates[static_cast<std::size_t>(plate)];
    return {id, area.x + 10.0F,
            area.y + area.height - StarfallSalvage::playerHeight,
            StarfallSalvage::encodeState(0, static_cast<int>(StarfallSalvage::relayPads.size()) + plate,
                                     0)};
}

bool supportedByDeck(const Rect& object)
{
    return std::any_of(StarfallSalvage::decks.begin(), StarfallSalvage::decks.end(),
                       [&object](const Rect& deck) {
                           return object.x >= deck.x &&
                                  object.x + object.width <= deck.x + deck.width &&
                                  object.y + object.height == deck.y;
                       });
}

// Frames the way the game runs them: the player's components, then the
// collision pass whose callbacks land, hurt or respawn the astronaut.
void stepPlayer(World& world, StarfallSalvage::Astronaut& astronaut, int frames)
{
    const FrameTime frame{1.0 / static_cast<double>(StarfallSalvage::normalLoopHz), 0};
    for (int i = 0; i < frames; ++i) {
        world.updateComponents(frame, "player");
        astronaut.beginCollisionPass();
        world.resolveCollisions();
        world.flushDestroyed();
    }
}

void placeAt(StarfallSalvage::Astronaut& astronaut, float x, float y)
{
    astronaut.spawn = {x, y, StarfallSalvage::playerWidth, StarfallSalvage::playerHeight};
    astronaut.respawn();
}

bool sameColor(const Color& a, const Color& b)
{
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

// The station as GameObjects, checked against the Part 2 requirements, and
// the astronaut's physics run through the engine's real collision pass.
int checkStation()
{
    World world;
    const StarfallSalvage::Station station = StarfallSalvage::buildStation(world);

    // At least three static platforms, each with its own position, size and
    // colour combination, and at least three distinct colours among them.
    const std::vector<GameObject*> decks = world.findAllByTag("deck");
    EXPECT(decks.size() >= 3);
    std::vector<Color> colors;
    for (std::size_t i = 0; i < decks.size(); ++i) {
        const Rect a = decks[i]->get<Transform>()->bounds();
        const Color colorA = decks[i]->get<Renderable>()->color;
        EXPECT(decks[i]->get<Collider>()->kind == Collider::Kind::Solid);
        for (std::size_t j = i + 1; j < decks.size(); ++j) {
            const Rect b = decks[j]->get<Transform>()->bounds();
            const bool sameBox = a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
            EXPECT(!(sameBox && sameColor(colorA, decks[j]->get<Renderable>()->color)));
        }
        if (std::none_of(colors.begin(), colors.end(), [&](const Color& c) { return sameColor(c, colorA); })) {
            colors.push_back(colorA);
        }
    }
    EXPECT(colors.size() >= 3);

    // Spawn points and death zones are hidden objects: no Renderable.
    EXPECT(station.spawns.size() >= 2);
    for (const GameObject* spawn : station.spawns) {
        EXPECT(!spawn->has<Renderable>());
        // On a deck, and not over the hull breach.
        const Rect r = spawn->get<Transform>()->bounds();
        EXPECT(supportedByDeck(r));
        EXPECT(!Collision::intersects({r.x, r.y, r.width, r.height + 1.0F}, StarfallSalvage::hullBreach));
    }
    // No spawn point lies anywhere a drone or shuttle travels, so nobody is
    // hit or pushed the moment they spawn or respawn.
    for (const Network::PlatformPath& path : StarfallSalvage::serverConfig().platforms) {
        Rect travel{};
        if (path.shape == Network::PlatformPath::Shape::Circular) {
            travel = {path.centerX - path.radius, path.centerY - path.radius,
                      2.0F * path.radius + path.width, 2.0F * path.radius + path.height};
        } else {
            travel = {std::min(path.startX, path.endX), std::min(path.startY, path.endY),
                      std::fabs(path.endX - path.startX) + path.width,
                      std::fabs(path.endY - path.startY) + path.height};
        }
        for (const GameObject* spawn : station.spawns) {
            EXPECT(!Collision::intersects(spawn->get<Transform>()->bounds(), travel));
        }
    }

    const std::vector<GameObject*> zones = world.findAllByTag("deathzone");
    EXPECT(zones.size() >= 1);
    for (const GameObject* zone : zones) {
        EXPECT(!zone->has<Renderable>());
        EXPECT(zone->get<Collider>()->kind == Collider::Kind::Trigger);
    }

    // At least two movement patterns among the moving objects; there are three.
    bool horizontal = false;
    bool vertical = false;
    bool circular = false;
    for (const Network::PlatformPath& path : StarfallSalvage::serverConfig().platforms) {
        if (path.shape == Network::PlatformPath::Shape::Circular) {
            circular = path.radius > 0.0F;
        } else if (path.startY == path.endY && path.startX != path.endX) {
            horizontal = true;
        } else if (path.startX == path.endX && path.startY != path.endY) {
            vertical = true;
        }
    }
    EXPECT(horizontal && vertical && circular);

    // The circling drone only threatens jumps: its whole orbit stays clear of
    // anyone standing on deck 2 (including on relay 2 or at cargo 3) and of
    // anyone riding the lift, even overhanging either edge.
    const Network::PlatformPath& circling = StarfallSalvage::dronePaths[1];
    const Network::PlatformPath& lift = StarfallSalvage::shuttlePaths[1];
    const Rect orbit{circling.centerX - circling.radius, circling.centerY - circling.radius,
                     2.0F * circling.radius + circling.width, 2.0F * circling.radius + circling.height};
    const Rect deck2 = StarfallSalvage::decks[2];
    const Rect standingOnDeck2{deck2.x - StarfallSalvage::playerWidth, deck2.y - StarfallSalvage::playerHeight,
                               deck2.width + 2.0F * StarfallSalvage::playerWidth, StarfallSalvage::playerHeight};
    const float liftTop = std::min(lift.startY, lift.endY);
    const Rect ridingLift{lift.startX - StarfallSalvage::playerWidth, liftTop - StarfallSalvage::playerHeight,
                          lift.width + 2.0F * StarfallSalvage::playerWidth,
                          std::fabs(lift.startY - lift.endY) + StarfallSalvage::playerHeight};
    EXPECT(!Collision::intersects(orbit, standingOnDeck2));
    EXPECT(!Collision::intersects(orbit, ridingLift));

    // The lift's whole travel stays clear of every deck.
    const Rect liftTravel{lift.startX, liftTop, lift.width,
                          std::fabs(lift.startY - lift.endY) + lift.height};
    for (const Rect& deck : StarfallSalvage::decks) EXPECT(!Collision::intersects(liftTravel, deck));

    GameObject& player = StarfallSalvage::makePlayer(world);
    StarfallSalvage::Astronaut& astronaut = *player.get<StarfallSalvage::Astronaut>();
    const Transform& body = *player.get<Transform>();
    const Motion& motion = *player.get<Motion>();

    // Standing on deck 1 for six seconds: grounded, and no drift either way.
    placeAt(astronaut, 76.0F, 380.0F);
    stepPlayer(world, astronaut, 300);
    EXPECT(astronaut.grounded);
    EXPECT(std::fabs(body.x - 76.0F) < 0.01F);
    EXPECT(std::fabs(body.y + body.height - StarfallSalvage::decks[1].y) <= 0.5F);

    // A jump and a double jump, but no third jump until landing again.
    astronaut.input.jump = true;
    stepPlayer(world, astronaut, 1);
    EXPECT(!astronaut.grounded && astronaut.jumpsUsed == 1 && motion.velocityY < 0.0F);
    stepPlayer(world, astronaut, 1);
    EXPECT(astronaut.jumpsUsed == 2);
    stepPlayer(world, astronaut, 1);
    EXPECT(astronaut.jumpsUsed == 2);
    astronaut.input.jump = false;
    stepPlayer(world, astronaut, 200);
    EXPECT(astronaut.grounded && astronaut.jumpsUsed == 0);

    // Falling from just under the roof, near terminal speed, with only half
    // the astronaut over deck 4's east edge still lands on the deck, not
    // beside it.
    const Rect deck4 = StarfallSalvage::decks[4];
    const float edgeX = deck4.x + deck4.width - StarfallSalvage::playerWidth * 0.5F;
    placeAt(astronaut, edgeX, StarfallSalvage::ceiling.y + StarfallSalvage::ceiling.height + 4.0F);
    stepPlayer(world, astronaut, 150);
    EXPECT(astronaut.grounded);
    EXPECT(std::fabs(body.x - edgeX) < 0.01F);
    EXPECT(std::fabs(body.y + body.height - deck4.y) <= 0.5F);

    // The closed bulkhead stops the astronaut at the wall; opened, which only
    // clears its collider's layers, it lets the astronaut walk through.
    Collider& gate = *station.bulkhead->get<Collider>();
    placeAt(astronaut, 880.0F, 460.0F);
    astronaut.input.axis = 1.0F;
    stepPlayer(world, astronaut, 100);
    EXPECT(std::fabs(body.x - (StarfallSalvage::bulkhead.x - StarfallSalvage::playerWidth)) < 0.01F);
    gate.layers = 0U;
    stepPlayer(world, astronaut, 30);
    EXPECT(body.x > StarfallSalvage::bulkhead.x + StarfallSalvage::bulkhead.width);
    gate.layers = StarfallSalvage::solidLayer;
    astronaut.input.axis = 0.0F;
    placeAt(astronaut, 76.0F, 380.0F);
    stepPlayer(world, astronaut, 5);

    // Riding a shuttle: carried by its move between frames, and let go if it
    // jumps further than a rider could have stayed on.
    GameObject& shuttle = StarfallSalvage::makeNetworkPlatform(world, {1, 0.0F, 0.0F, 0.0F, 0.0F});
    shuttle.add<NetworkIdentity>(NetworkIdentity::Kind::Platform, 1U);
    Transform& shuttlePose = *shuttle.get<Transform>();
    shuttlePose.x = 300.0F;
    shuttlePose.y = 300.0F;
    shuttlePose.width = 112.0F;
    shuttlePose.height = 18.0F;
    shuttle.setActive(true);
    placeAt(astronaut, 340.0F, 300.0F - StarfallSalvage::playerHeight - 1.0F);
    stepPlayer(world, astronaut, 5);
    EXPECT(astronaut.grounded && astronaut.supportId == 1);
    shuttlePose.x += 5.0F;
    astronaut.rideShuttle(&shuttle);
    EXPECT(std::fabs(body.x - 345.0F) < 0.01F);
    stepPlayer(world, astronaut, 1);
    EXPECT(astronaut.supportId == 1);
    shuttlePose.x += 100.0F;
    astronaut.rideShuttle(&shuttle);
    EXPECT(astronaut.supportId == 0 && std::fabs(body.x - 345.0F) < 0.01F);

    // Riding the lift up and back down: the astronaut's feet stay on its top
    // surface every frame, moved the way NetworkWorldSync would move it.
    GameObject& liftObject = StarfallSalvage::makeNetworkPlatform(world, {2, 0.0F, 0.0F, 0.0F, 0.0F});
    liftObject.add<NetworkIdentity>(NetworkIdentity::Kind::Platform, 2U);
    Transform& liftPose = *liftObject.get<Transform>();
    liftPose.x = lift.startX;
    liftPose.y = lift.startY;
    liftPose.width = lift.width;
    liftPose.height = lift.height;
    liftObject.setActive(true);
    placeAt(astronaut, lift.startX + 15.0F, lift.startY - StarfallSalvage::playerHeight - 1.0F);
    stepPlayer(world, astronaut, 5);
    EXPECT(astronaut.grounded && astronaut.supportId == 2);
    const float liftStep = lift.speed / static_cast<float>(StarfallSalvage::normalLoopHz);
    for (int frame = 0; frame < 90; ++frame) {
        liftPose.y += frame < 60 ? -liftStep : liftStep;
        astronaut.rideShuttle(&liftObject);
        stepPlayer(world, astronaut, 1);
        EXPECT(astronaut.grounded && astronaut.supportId == 2);
        EXPECT(std::fabs(body.y + body.height - liftPose.y) <= 0.5F);
    }
    return 0;
}

// Flipped gravity: the astronaut falls to the ceiling, stands on its
// underside, jumps downward, and falls back when gravity is restored.
int checkGravity()
{
    World world;
    StarfallSalvage::buildStation(world);
    GameObject& player = StarfallSalvage::makePlayer(world);
    StarfallSalvage::Astronaut& astronaut = *player.get<StarfallSalvage::Astronaut>();
    const Transform& body = *player.get<Transform>();
    const Motion& motion = *player.get<Motion>();
    const float roof = StarfallSalvage::ceiling.y + StarfallSalvage::ceiling.height;

    placeAt(astronaut, 76.0F, 380.0F);
    stepPlayer(world, astronaut, 30);
    EXPECT(astronaut.grounded);

    astronaut.setFlipped(true);
    EXPECT(astronaut.flipped() && !astronaut.grounded);
    stepPlayer(world, astronaut, 150);
    EXPECT(astronaut.grounded);
    EXPECT(std::fabs(body.y - roof) <= 0.5F);

    // A jump pushes away from the ceiling, so downward.
    astronaut.input.jump = true;
    stepPlayer(world, astronaut, 1);
    astronaut.input.jump = false;
    EXPECT(!astronaut.grounded && astronaut.jumpsUsed == 1 && motion.velocityY > 0.0F);
    stepPlayer(world, astronaut, 150);
    EXPECT(astronaut.grounded && std::fabs(body.y - roof) <= 0.5F);

    astronaut.setFlipped(false);
    stepPlayer(world, astronaut, 150);
    EXPECT(astronaut.grounded);
    EXPECT(std::fabs(body.y + body.height - StarfallSalvage::decks[1].y) <= 0.5F);

    // Respawning always restores normal gravity.
    astronaut.setFlipped(true);
    astronaut.respawn();
    EXPECT(!astronaut.flipped());
    return 0;
}

// The station's gameplay through the engine's events: deaths, spawns, crates,
// gravity panels, reviving and the server's alarms.
int checkEvents()
{
    TestClock clock;
    EventManager events(clock);
    World world;
    const StarfallSalvage::Station layout = StarfallSalvage::buildStation(world);
    world.setEventManager(&events);
    GameObject& player = StarfallSalvage::makePlayer(world);
    StarfallSalvage::Astronaut& astronaut = *player.get<StarfallSalvage::Astronaut>();
    const Transform& body = *player.get<Transform>();

    std::uint8_t collected = 0;
    Rect rescuerBounds{600.0F, 460.0F, StarfallSalvage::playerWidth, StarfallSalvage::playerHeight};
    StarfallSalvage::StationEvents::Hooks hooks;
    hooks.collectCargo = [&collected](std::size_t crate) {
        const auto bit = static_cast<std::uint8_t>(1U << crate);
        const bool fresh = (collected & bit) == 0;
        collected |= bit;
        return fresh;
    };
    hooks.findPlayer = [&rescuerBounds](std::uint32_t id, Rect& bounds) {
        if (id != 7) return false;
        bounds = rescuerBounds;
        return true;
    };
    hooks.localPlayerId = [] { return 1U; };
    // Player 4 is the alarm director here (in the game: the lowest id online).
    hooks.alarmDirectorId = [] { return 4U; };
    StarfallSalvage::StationEvents station(events, player, layout, hooks);

    std::vector<std::uint32_t> revivesRaised;
    events.subscribe(StarfallSalvage::ReviveEvent, [&revivesRaised](const Event& event) {
        if (event.origin == 0) revivesRaised.push_back(static_cast<std::uint32_t>(event.getOr<std::int64_t>("target", 0)));
    });

    bool reviveHeld = false;
    std::vector<StarfallSalvage::Claim> teammates;
    // One frame as the game runs it: physics only while not down, then the
    // queued events, then the timers.
    const auto step = [&](int frames) {
        const FrameTime frame{1.0 / static_cast<double>(StarfallSalvage::normalLoopHz), 0};
        for (int i = 0; i < frames; ++i) {
            clock.elapsed += kGameTicsPerSecond / StarfallSalvage::normalLoopHz;
            if (!station.down()) {
                world.updateComponents(frame, "player");
                astronaut.beginCollisionPass();
                world.resolveCollisions();
            }
            world.flushDestroyed();
            events.dispatch();
            station.update(static_cast<float>(frame.dtSeconds), reviveHeld, teammates);
            events.dispatch();
        }
    };
    const auto placeDrone = [&](Transform& pose) {
        pose.x = body.x - 5.0F;
        pose.y = body.y + 10.0F;
        pose.width = 46.0F;
        pose.height = 26.0F;
    };
    const auto spawnAt = [&](float x, float y) {
        Event spawn(Events::Spawn);
        spawn.set("x", static_cast<double>(x)).set("y", static_cast<double>(y));
        events.raise(std::move(spawn));
        events.dispatch();
    };

    // The breach: a DeathEvent that respawns without costing a life.
    spawnAt(76.0F, 380.0F);
    step(5);
    Transform& movable = *player.get<Transform>();
    movable.x = StarfallSalvage::hullBreach.x + 25.0F;
    movable.y = StarfallSalvage::decks[2].y + StarfallSalvage::decks[2].height + 20.0F;
    step(150);
    EXPECT(std::fabs(body.x - astronaut.spawn.x) < 0.01F);
    EXPECT(astronaut.lives == 3 && !station.down());

    // Touching a crate collects it once.
    const Rect crate0 = StarfallSalvage::cargo[0];
    spawnAt(crate0.x, crate0.y - 10.0F);
    step(2);
    EXPECT(collected == 0x1);

    // A floor panel flips gravity up to the ceiling; walking the ceiling picks
    // up the crate hanging over the breach; the ceiling panel flips it back.
    const Rect floorPanel = StarfallSalvage::gravityPanels[0];
    spawnAt(floorPanel.x + 5.0F, floorPanel.y + floorPanel.height - StarfallSalvage::playerHeight - 2.0F);
    step(3);
    EXPECT(astronaut.flipped() && station.flags().flipped);
    step(120);
    const float roof = StarfallSalvage::ceiling.y + StarfallSalvage::ceiling.height;
    EXPECT(astronaut.grounded && std::fabs(body.y - roof) <= 0.5F);
    astronaut.input.axis = 1.0F;
    for (int i = 0; i < 300 && astronaut.flipped(); ++i) step(1);
    astronaut.input.axis = 0.0F;
    EXPECT(!astronaut.flipped());
    EXPECT((collected & 0x2) != 0);
    step(120);
    EXPECT(astronaut.grounded && std::fabs(body.y + body.height - StarfallSalvage::decks[6].y) <= 0.5F);

    // A drone: a life lost, then down, frozen, with a ten second beacon.
    spawnAt(76.0F, 380.0F);
    step(60);
    astronaut.invulnerableSeconds = 0.0F;
    GameObject& drone = StarfallSalvage::makeNetworkPlatform(world, {11, 0.0F, 0.0F, 0.0F, 0.0F});
    Transform& dronePose = *drone.get<Transform>();
    placeDrone(dronePose);
    drone.setActive(true);
    step(1);
    drone.setActive(false);
    EXPECT(station.down() && astronaut.lives == 2 && station.hasBeacon());
    EXPECT(station.flags().down);
    const float downX = body.x;
    step(static_cast<int>((StarfallSalvage::beaconSeconds - 0.2F) * StarfallSalvage::normalLoopHz));
    EXPECT(station.down() && std::fabs(body.x - downX) < 0.01F);
    step(20);
    EXPECT(!station.down() && std::fabs(body.x - astronaut.spawn.x) < 0.01F && astronaut.invulnerableSeconds > 0.0F);

    // A shield that runs out while still inside a drone ends in a hit: drones
    // raise "stay" collisions, not only "enter".
    step(60);
    astronaut.invulnerableSeconds = 0.3F;
    drone.setActive(true);
    placeDrone(dronePose);
    step(5);
    EXPECT(!station.down() && astronaut.lives == 2);
    step(20);
    drone.setActive(false);
    EXPECT(station.down() && astronaut.lives == 1);

    // A revive from a player across the station is refused; one from beside
    // this player brings it back at the rescuer's side.
    const auto reviveFrom = [&](std::uint32_t rescuer) {
        Event revive(StarfallSalvage::ReviveEvent);
        revive.set("target", static_cast<std::int64_t>(1));
        revive.origin = rescuer;
        events.raise(std::move(revive));
        events.dispatch();
    };
    rescuerBounds.x = body.x + 400.0F;
    rescuerBounds.y = body.y;
    reviveFrom(7);
    EXPECT(station.down());
    reviveFrom(8); // a player NetworkWorldSync does not know
    EXPECT(station.down());
    rescuerBounds.x = body.x + 30.0F;
    reviveFrom(7);
    EXPECT(!station.down());
    EXPECT(std::fabs(body.x - rescuerBounds.x) < 0.01F && std::fabs(body.y - rescuerBounds.y) < 0.01F);
    EXPECT(astronaut.invulnerableSeconds >= 1.9F);
    EXPECT(station.banner().kind == StarfallSalvage::Banner::Kind::Revived && station.banner().subject == 0 &&
           station.banner().actor == 7);

    // With no lives left there is no beacon: only a revive, or restarting alone.
    step(60);
    astronaut.invulnerableSeconds = 0.0F;
    drone.setActive(true);
    placeDrone(dronePose);
    step(1);
    drone.setActive(false);
    EXPECT(station.down() && astronaut.lives == 0 && !station.hasBeacon());
    step(static_cast<int>((StarfallSalvage::beaconSeconds + 2.0F) * StarfallSalvage::normalLoopHz));
    EXPECT(station.down());
    station.restartAlone();
    events.dispatch();
    EXPECT(!station.down() && astronaut.lives == 3);

    // Rescuing: holding the revive chord next to a downed teammate for long
    // enough raises one ReviveEvent naming it; one not down is ignored.
    spawnAt(100.0F, 380.0F);
    step(30);
    teammates = {{5, body.x + 20.0F, body.y, StarfallSalvage::encodeState(0, -1, 0, {false, true})}};
    reviveHeld = true;
    step(static_cast<int>(StarfallSalvage::reviveSeconds * StarfallSalvage::normalLoopHz) - 5);
    EXPECT(revivesRaised.empty() && station.reviveTargetId() == 5 && station.reviveProgress() > 0.5F);
    step(10);
    EXPECT(revivesRaised.size() == 1 && revivesRaised[0] == 5);
    reviveHeld = false;
    teammates = {{5, body.x + 20.0F, body.y, StarfallSalvage::encodeState(0, -1, 0, {false, false})}};
    EXPECT(StarfallSalvage::reviveTarget(body.bounds(), 1, teammates) == 0);
    teammates = {{5, body.x + 400.0F, body.y, StarfallSalvage::encodeState(0, -1, 0, {false, true})}};
    EXPECT(StarfallSalvage::reviveTarget(body.bounds(), 1, teammates) == 0);
    teammates.clear();

    // Another player's death only shows a banner.
    Event remoteDeath(Events::Death);
    remoteDeath.set("cause", "drone");
    remoteDeath.origin = 9;
    events.raise(std::move(remoteDeath));
    events.dispatch();
    EXPECT(!station.down() && station.banner().kind == StarfallSalvage::Banner::Kind::Down &&
           station.banner().subject == 9);

    // The director's alarm warns first, then flips gravity for its duration,
    // both on game time; panels do nothing meanwhile. Bad alarms are ignored.
    Event alarm(StarfallSalvage::StationAlarmEvent);
    alarm.set("kind", "gravity").set("warning", 2.0).set("duration", 6.0);
    alarm.origin = 4;
    events.raise(std::move(alarm));
    events.dispatch();
    EXPECT(station.alarmWarning() == "gravity" && !station.gravityFailure());
    step(95);
    EXPECT(!station.gravityFailure() && !astronaut.flipped());
    step(10);
    EXPECT(station.gravityFailure() && astronaut.flipped() && station.alarmWarning().empty());
    step(6 * StarfallSalvage::normalLoopHz);
    EXPECT(!station.gravityFailure() && !astronaut.flipped());

    // A player who is not the director cannot set off an alarm.
    Event forged(StarfallSalvage::StationAlarmEvent);
    forged.set("kind", "gravity").set("warning", 0.0).set("duration", 6.0);
    forged.origin = 5;
    events.raise(std::move(forged));
    events.dispatch();
    step(5);
    EXPECT(station.alarmWarning().empty() && !station.gravityFailure());

    Event bogus(StarfallSalvage::StationAlarmEvent);
    bogus.set("kind", "meteors");
    bogus.origin = 4;
    events.raise(std::move(bogus));
    Event blackout(StarfallSalvage::StationAlarmEvent);
    blackout.set("kind", "blackout").set("warning", "soon").set("duration", static_cast<std::int64_t>(1));
    blackout.origin = 4;
    events.raise(std::move(blackout));
    events.dispatch();
    EXPECT(station.alarmWarning() == "blackout");
    step(2 * StarfallSalvage::normalLoopHz + 5);
    EXPECT(station.blackout());
    step(StarfallSalvage::normalLoopHz);
    EXPECT(!station.blackout());

    // This player's own alarm (it is the director in its own game) counts.
    Event own(StarfallSalvage::StationAlarmEvent);
    own.set("kind", "blackout").set("warning", 0.0).set("duration", 1.0);
    events.raise(std::move(own));
    events.dispatch();
    step(2);
    EXPECT(station.blackout());
    step(StarfallSalvage::normalLoopHz);
    EXPECT(!station.blackout());
    return 0;
}

} // namespace

int main()
{
    using StarfallSalvage::evaluateMission;

    std::uint8_t mask = 0;
    std::uint8_t personalMask = 0;
    int pad = -1;
    EXPECT(StarfallSalvage::encodeState(0xA, 2, 0x2) == "CAP2I2G0D0");
    EXPECT(StarfallSalvage::encodeState(0xF, -1, 0) == "CFP-I0G0D0");
    EXPECT(StarfallSalvage::encodeState(0x1, -1, 0x1, {true, true}) == "C1P-I1G1D1");
    EXPECT(StarfallSalvage::decodeState("C3P0I1G0D0", mask, pad, personalMask) &&
           mask == 3 && pad == 0 && personalMask == 1);
    EXPECT(StarfallSalvage::decodeState("CFP-I0G0D0", mask, pad, personalMask) &&
           mask == 15 && pad == -1 && personalMask == 0);
    EXPECT(StarfallSalvage::decodeState("C0P3I0G0D0", mask, pad, personalMask) && pad == 3);
    EXPECT(StarfallSalvage::decodeState("C0P4I0G0D0", mask, pad, personalMask) && pad == 4);
    EXPECT(!StarfallSalvage::decodeState("P0", mask, pad, personalMask));
    EXPECT(!StarfallSalvage::decodeState("C3P0", mask, pad, personalMask));
    EXPECT(!StarfallSalvage::decodeState("C3P0I1", mask, pad, personalMask)); // the old six-character form
    EXPECT(!StarfallSalvage::decodeState("C0P5I0G0D0", mask, pad, personalMask));
    EXPECT(!StarfallSalvage::decodeState("C0P-I1G0D0", mask, pad, personalMask));
    EXPECT(!StarfallSalvage::decodeState("CgP0I0G0D0", mask, pad, personalMask));
    EXPECT(!StarfallSalvage::decodeState("C3P0I1G2D0", mask, pad, personalMask));
    EXPECT(!StarfallSalvage::decodeState("CFP0extra!", mask, pad, personalMask));
    EXPECT(StarfallSalvage::crewFlags("C1P-I1G1D0").flipped && !StarfallSalvage::crewFlags("C1P-I1G1D0").down);
    EXPECT(StarfallSalvage::crewFlags("C1P-I1G0D1").down);
    EXPECT(!StarfallSalvage::crewFlags("garbage").down);

    std::vector<StarfallSalvage::Claim> players{
        onPad(1, 0, 0x1), onPad(2, 1, 0x2), onPad(3, 2, 0xC)
    };
    auto result = evaluateMission(players);
    EXPECT(result.complete);
    EXPECT(result.cargoMask == StarfallSalvage::allCargoMask);
    EXPECT(result.relays.occupied[0] && result.relays.occupied[1] && result.relays.occupied[2]);
    EXPECT(StarfallSalvage::chooseCargoWinner(players).playerId == 3);
    EXPECT(StarfallSalvage::chooseCargoWinner(players).crates == 2);

    const std::vector<StarfallSalvage::Claim> simultaneousPickup{
        {4, 0.0F, 0.0F, StarfallSalvage::encodeState(0x1, -1, 0x1)},
        {2, 0.0F, 0.0F, StarfallSalvage::encodeState(0x3, -1, 0x3)},
        {3, 0.0F, 0.0F, StarfallSalvage::encodeState(0x4, -1, 0x4)},
    };
    EXPECT(StarfallSalvage::chooseCargoWinner(simultaneousPickup).playerId == 2);
    EXPECT(StarfallSalvage::chooseCargoWinner(simultaneousPickup).crates == 2);
    EXPECT(StarfallSalvage::creditedCargoMask(simultaneousPickup, 2) == 0x3);
    EXPECT(StarfallSalvage::creditedCargoMask(simultaneousPickup, 4) == 0);
    EXPECT(StarfallSalvage::creditedCargoMask(simultaneousPickup, 3) == 0x4);
    EXPECT(StarfallSalvage::creditedCargoMask({simultaneousPickup[0], simultaneousPickup[1]}, 2) == 0x3);
    EXPECT(StarfallSalvage::creditedCargoMask({simultaneousPickup[1], simultaneousPickup[0]}, 2) == 0x3);

    EXPECT(StarfallSalvage::chooseCargoWinner({onPad(9, 0, 1), onPad(2, 1, 2)}).playerId == 2);
    EXPECT(StarfallSalvage::chooseCargoWinner({{1, 0.0F, 0.0F, "bad"}, onPad(2, 1, 2)}).playerId == 2);
    EXPECT(StarfallSalvage::chooseCargoWinner({}).playerId == 0);

    players.pop_back();
    EXPECT(!evaluateMission(players).complete);
    players.push_back(onPad(2, 2, 0xC)); // duplicate ID cannot operate two relays
    EXPECT(!evaluateMission(players).complete);
    players.back() = onPad(3, 2, 0xC);
    players.back().x = 40.0F; // a stale or forged pad claim is not enough
    EXPECT(!evaluateMission(players).complete);
    players.back() = onPad(3, 2, 0xC);
    players.back().data = "garbage";
    EXPECT(!evaluateMission(players).complete);
    players.back() = onPad(3, 2, 0x4);
    EXPECT(evaluateMission(players).relays.online);
    EXPECT(!evaluateMission(players).complete); // all relays are held, but cargo is missing
    players.back() = onPad(3, 2, 0xC);
    EXPECT(evaluateMission(players).complete);

    const auto lateJoinView = evaluateMission({onPad(1, 0, StarfallSalvage::allCargoMask)});
    EXPECT(lateJoinView.cargoMask == StarfallSalvage::allCargoMask);
    EXPECT(!lateJoinView.complete); // progress survives in a peer, not a lone relay

    players.erase(players.begin() + 1); // leave/timeout removes the claim
    EXPECT(!evaluateMission(players).complete);
    EXPECT(StarfallSalvage::padUnderPlayer({40.0F, 360.0F, 30.0F, 40.0F}) == -1);
    EXPECT(StarfallSalvage::padUnderPlayer({onPad(1, 0, 0).x, onPad(1, 0, 0).y,
                                       StarfallSalvage::playerWidth,
                                       StarfallSalvage::playerHeight}) == 0);

    auto gateClaim = onGate(1, 0);
    EXPECT(evaluateMission({gateClaim}).gateOpen);
    EXPECT(!evaluateMission({}).gateOpen);
    gateClaim.x = 40.0F;
    EXPECT(!evaluateMission({gateClaim}).gateOpen);
    gateClaim = onGate(2, 1);
    EXPECT(evaluateMission({gateClaim}).gateOpen);
    gateClaim.data = "C0P5I0";
    EXPECT(!evaluateMission({gateClaim}).gateOpen);
    EXPECT(StarfallSalvage::gatePlateUnderPlayer({onGate(2, 1).x, onGate(2, 1).y,
                                             StarfallSalvage::playerWidth,
                                             StarfallSalvage::playerHeight}) == 4);

    EXPECT(StarfallSalvage::frameIntervalNs(0.5) == 40'000'000);
    EXPECT(StarfallSalvage::frameIntervalNs(1.0) == 20'000'000);
    EXPECT(StarfallSalvage::frameIntervalNs(2.0) == 10'000'000);
    EXPECT(StarfallSalvage::frameIntervalNs(0.0) == 20'000'000);
    EXPECT(StarfallSalvage::frameIntervalNs(-1.0) == 20'000'000);
    EXPECT(StarfallSalvage::frameIntervalNs(std::numeric_limits<double>::infinity()) == 20'000'000);
    EXPECT(StarfallSalvage::frameIntervalNs(std::numeric_limits<double>::quiet_NaN()) == 20'000'000);
    EXPECT(checkTimeAndPacing() == 0);

    const auto config = StarfallSalvage::serverConfig();
    EXPECT(config.platforms.size() == 5);
    EXPECT(config.spawnPoints.size() == 8);
    EXPECT(config.platforms[0].endX > config.platforms[0].startX);
    // Shuttle 2 is a lift: same x, different heights.
    EXPECT(config.platforms[1].endX == config.platforms[1].startX);
    EXPECT(config.platforms[1].endY < config.platforms[1].startY);
    EXPECT(StarfallSalvage::isShuttle(config.platforms[0].id));
    EXPECT(StarfallSalvage::isDrone(config.platforms[2].id));
    EXPECT(StarfallSalvage::isDrone(config.platforms[3].id));
    EXPECT(StarfallSalvage::isDrone(config.platforms[4].id));
    EXPECT(config.platforms[2].endX - config.platforms[2].startX >= 300.0F);
    EXPECT(config.platforms[4].endX - config.platforms[4].startX >= 400.0F);
    // Every crate sits on a deck, except the one hanging under the roof.
    for (const Rect& crate : StarfallSalvage::cargo) {
        EXPECT(supportedByDeck(crate) ||
               crate.y == StarfallSalvage::ceiling.y + StarfallSalvage::ceiling.height);
    }
    for (const Rect& relay : StarfallSalvage::relayPads) EXPECT(supportedByDeck(relay));
    for (const Rect& plate : StarfallSalvage::gatePlates) EXPECT(supportedByDeck(plate));
    EXPECT(StarfallSalvage::bulkhead.x > StarfallSalvage::decks[3].x + StarfallSalvage::decks[3].width);
    EXPECT(StarfallSalvage::bulkhead.x + StarfallSalvage::bulkhead.width < StarfallSalvage::decks[4].x);
    EXPECT(StarfallSalvage::worldWidth > StarfallSalvage::width);
    EXPECT(StarfallSalvage::cameraTargetX(0.0F) == 0.0F);
    EXPECT(StarfallSalvage::cameraTargetX(800.0F) == 335.0F);
    EXPECT(StarfallSalvage::cameraTargetX(1550.0F) == 640.0F);
    EXPECT(StarfallSalvage::cameraTargetX(2000.0F) == 640.0F);

    EXPECT(checkStation() == 0);
    EXPECT(checkGravity() == 0);
    EXPECT(checkEvents() == 0);

    std::cout << "Starfall Salvage cooperative rules, time/pacing, station, gravity and event checks passed\n";
}
