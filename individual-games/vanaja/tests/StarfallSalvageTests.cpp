#include "StarfallSalvageLevel.hpp"
#include "StarfallSalvageRules.hpp"
#include "StarfallSalvageWorld.hpp"
#include "Components.hpp"
#include "DeltaTimer.hpp"
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

    // Falling from high above at terminal speed with only half the astronaut
    // over deck 4's east edge still lands on the deck, not beside it.
    const Rect deck4 = StarfallSalvage::decks[4];
    const float edgeX = deck4.x + deck4.width - StarfallSalvage::playerWidth * 0.5F;
    placeAt(astronaut, edgeX, -200.0F);
    stepPlayer(world, astronaut, 150);
    EXPECT(astronaut.grounded);
    EXPECT(std::fabs(body.x - edgeX) < 0.01F);
    EXPECT(std::fabs(body.y + body.height - deck4.y) <= 0.5F);

    // Into the hull breach, which is reached along the floor under deck 2: the
    // hidden death zone sends the astronaut back to its spawn point without
    // costing a life.
    placeAt(astronaut, 76.0F, 380.0F);
    Transform& movable = *player.get<Transform>();
    movable.x = StarfallSalvage::hullBreach.x + 25.0F;
    movable.y = StarfallSalvage::decks[2].y + StarfallSalvage::decks[2].height + 20.0F;
    stepPlayer(world, astronaut, 150);
    EXPECT(std::fabs(body.x - 76.0F) < 0.01F);
    EXPECT(astronaut.lives == 3);

    // A drone costs one life and respawns the astronaut, then a second of
    // invulnerability protects it even though it respawned inside the drone.
    GameObject& drone = StarfallSalvage::makeNetworkPlatform(world, {11, 0.0F, 0.0F, 0.0F, 0.0F});
    Transform& dronePose = *drone.get<Transform>();
    dronePose.x = 70.0F;
    dronePose.y = 390.0F;
    dronePose.width = 46.0F;
    dronePose.height = 26.0F;
    drone.setActive(true);
    stepPlayer(world, astronaut, 1);
    EXPECT(astronaut.lives == 2 && astronaut.invulnerableSeconds > 0.0F);
    stepPlayer(world, astronaut, 10);
    EXPECT(astronaut.lives == 2);
    stepPlayer(world, astronaut, 60);
    EXPECT(astronaut.lives == 1);
    drone.setActive(false);

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
    return 0;
}

} // namespace

int main()
{
    using StarfallSalvage::evaluateMission;

    std::uint8_t mask = 0;
    std::uint8_t personalMask = 0;
    int pad = -1;
    EXPECT(StarfallSalvage::encodeState(0xA, 2, 0x2) == "CAP2I2");
    EXPECT(StarfallSalvage::encodeState(0xF, -1, 0) == "CFP-I0");
    EXPECT(StarfallSalvage::decodeState("C3P0I1", mask, pad, personalMask) &&
           mask == 3 && pad == 0 && personalMask == 1);
    EXPECT(StarfallSalvage::decodeState("CFP-I0", mask, pad, personalMask) &&
           mask == 15 && pad == -1 && personalMask == 0);
    EXPECT(StarfallSalvage::decodeState("C0P3I0", mask, pad, personalMask) && pad == 3);
    EXPECT(StarfallSalvage::decodeState("C0P4I0", mask, pad, personalMask) && pad == 4);
    EXPECT(!StarfallSalvage::decodeState("P0", mask, pad, personalMask));
    EXPECT(!StarfallSalvage::decodeState("C3P0", mask, pad, personalMask));
    EXPECT(!StarfallSalvage::decodeState("C0P5I0", mask, pad, personalMask));
    EXPECT(!StarfallSalvage::decodeState("C0P-I1", mask, pad, personalMask));
    EXPECT(!StarfallSalvage::decodeState("CgP0I0", mask, pad, personalMask));
    EXPECT(!StarfallSalvage::decodeState("CFP0extra", mask, pad, personalMask));

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

    const Rect firstCrate = StarfallSalvage::cargo[0];
    EXPECT(StarfallSalvage::cargoAt(firstCrate, 0, true, 0.02F) == 0x1);
    EXPECT(StarfallSalvage::cargoAt(firstCrate, 0, false, 0.02F) == 0);
    EXPECT(StarfallSalvage::cargoAt(firstCrate, 0, true, 0.0F) == 0);
    EXPECT(StarfallSalvage::cargoAt(firstCrate, 0x1, true, 0.02F) == 0);
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
    for (const Rect& crate : StarfallSalvage::cargo) EXPECT(supportedByDeck(crate));
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

    std::cout << "Starfall Salvage cooperative rules, time/pacing and station checks passed\n";
}
