#include "StarfallSalvageLevel.hpp"

#include "StarfallSalvageEvents.hpp"
#include "StarfallSalvageWorld.hpp"
#include "TimeUnits.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <variant>

namespace StarfallSalvage {
namespace {

constexpr Color cargoColor{213, 112, 47};
constexpr Color relayIdleColor{70, 104, 134};
constexpr Color gatePlateColor{247, 180, 93};
constexpr Color shuttleColor{44, 70, 100};
constexpr Color gravityPanelColor{176, 120, 255};

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
        return {{66, 58, 108}, {168, 150, 232}, {100, 88, 158}, {124, 110, 180}};
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
    gravity_ = &owner().require<Gravity>();
    Collider& collider = owner().require<Collider>();
    // Landing has to be known within the same collision pass, every frame,
    // so it stays a callback; gameplay touches are CollisionEvents.
    collider.onCollide = [this](GameObject&, GameObject& other) { touch(other); };
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
        // A jump pushes against gravity, whichever way it pulls.
        motion_->velocityY = flipped_ ? jumpSpeed : -jumpSpeed;
        ++jumpsUsed;
        grounded = false;
        supportId = 0;
    }
    motion_->velocityY = flipped_ ? std::max(motion_->velocityY, -terminalFallSpeed)
                                  : std::min(motion_->velocityY, terminalFallSpeed);
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
    respawnAt(spawn.x, spawn.y);
}

void Astronaut::respawnAt(float x, float y)
{
    setFlipped(false);
    transform_->x = x;
    transform_->y = y;
    motion_->velocityX = 0.0F;
    motion_->velocityY = 0.0F;
    grounded = false;
    jumpsUsed = 0;
    supportId = 0;
}

void Astronaut::setFlipped(bool flipped)
{
    if (flipped != flipped_) {
        grounded = false;
        supportId = 0;
        jumpsUsed = 0;
    }
    flipped_ = flipped;
    gravity_->acceleration = flipped ? -gravity : gravity;
}

void Astronaut::touch(GameObject& other)
{
    const std::string& tag = other.tag();
    if (tag != "deck" && tag != "shuttle" && tag != "bulkhead") {
        return;
    }

    // The collision pass has already pushed the astronaut out. It is standing
    // on `other` when that push left its feet exactly on the top surface, or,
    // with gravity flipped, its head exactly on the underside.
    const Rect self = transform_->bounds();
    const Rect surface = other.get<Transform>()->bounds();
    const bool overlapsX = self.x < surface.x + surface.width && self.x + self.width > surface.x;
    const bool standing =
        flipped_ ? std::fabs(self.y - (surface.y + surface.height)) <= 0.5F && motion_->velocityY <= 0.0F
                 : std::fabs(self.y + self.height - surface.y) <= 0.5F && motion_->velocityY >= 0.0F;
    if (!overlapsX || !standing) {
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

GameObject& makeCargo(World& world, const Rect& box)
{
    GameObject& crate = makeFixture(world, "cargo", box, cargoColor);
    crate.add<Collider>(Collider::Kind::Trigger, pickupLayer);
    return crate;
}

GameObject& makeGravityPanel(World& world, const Rect& box)
{
    GameObject& panel = makeFixture(world, "gravitypanel", box, gravityPanelColor);
    panel.add<Collider>(Collider::Kind::Trigger, zoneLayer);
    return panel;
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
    player.add<Collider>(Collider::Kind::Solid, solidLayer | hazardLayer | zoneLayer | pickupLayer);
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
        // "stay" events too, so a shield that runs out inside a drone still
        // ends in a hit.
        drone.add<Collider>(Collider::Kind::Trigger, hazardLayer).raiseStayEvents = true;
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
    makeDeck(world, ceiling, DeckKind::Hull);
    for (const Network::SpawnPoint& spawn : spawns) {
        station.spawns.push_back(&makeSpawnPoint(world, spawn.x, spawn.y));
    }
    for (const Rect& zone : deathZones) {
        makeDeathZone(world, zone);
    }
    for (const Rect& crate : cargo) {
        station.cargo.push_back(&makeCargo(world, crate));
    }
    for (const Rect& panel : gravityPanels) {
        station.gravityPanels.push_back(&makeGravityPanel(world, panel));
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

// --- StationEvents (declared in StarfallSalvageEvents.hpp) -------------------
// Kept in this file so the game adds no source to the shared build.
namespace {

// Stops a panel from flipping an astronaut back and forth while it stands on
// the panel's edge.
constexpr float panelCooldownSeconds = 0.4F;
constexpr float bannerSeconds = 3.0F;
// How far a rescuer may be from this player when its ReviveEvent arrives:
// reviveRange, plus slack for the rescuer having moved since it was checked.
constexpr float reviveAcceptRange = reviveRange * 2.0F;

// Spawning is what ends being down, so it must never be the event a full
// queue drops.
Event spawnEvent(const char* cause)
{
    Event spawn(Events::Spawn);
    spawn.priority = EventPriority::High;
    spawn.set("cause", cause);
    return spawn;
}

// An alarm usually comes from another player, so its numbers are clamped rather than
// trusted, and may be written as integers or decimals.
double seconds(const Event& event, const char* name, double fallback, double low, double high)
{
    const auto found = event.args.find(name);
    double value = fallback;
    if (found != event.args.end()) {
        if (const auto* decimal = std::get_if<double>(&found->second)) {
            value = *decimal;
        } else if (const auto* integer = std::get_if<std::int64_t>(&found->second)) {
            value = static_cast<double>(*integer);
        }
    }
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

std::int64_t tics(double secondsValue)
{
    return static_cast<std::int64_t>(secondsValue * static_cast<double>(kGameTicsPerSecond));
}

} // namespace

StationEvents::StationEvents(EventManager& events, GameObject& player, const Station& station, Hooks hooks)
    : events_(events),
      player_(player),
      astronaut_(*player.get<Astronaut>()),
      station_(station),
      hooks_(std::move(hooks))
{
    subscriptions_ = {
        events_.subscribe(Events::Collision, [this](const Event& event) { onCollision(event); }),
        events_.subscribe(Events::Death, [this](const Event& event) { onDeath(event); }),
        events_.subscribe(Events::Spawn, [this](const Event& event) { onSpawn(event); }),
        events_.subscribe(ReviveEvent, [this](const Event& event) { onRevive(event); }),
        events_.subscribe(StationAlarmEvent, [this](const Event& event) { onAlarm(event); }),
        events_.subscribe(AlarmBeginEvent, [this](const Event& event) { onAlarmEdge(event, 1); }),
        events_.subscribe(AlarmEndEvent, [this](const Event& event) { onAlarmEdge(event, -1); }),
    };
}

StationEvents::~StationEvents()
{
    for (const EventManager::SubscriptionId id : subscriptions_) {
        events_.unsubscribe(id);
    }
}

void StationEvents::update(float dt, bool reviveHeld, const std::vector<Claim>& teammates)
{
    if (dt <= 0.0F) {
        return;
    }
    panelCooldown_ = std::max(0.0F, panelCooldown_ - dt);
    banner_.seconds = std::max(0.0F, banner_.seconds - dt);
    warningSeconds_ = std::max(0.0F, warningSeconds_ - dt);

    if (down_) {
        reviveTarget_ = 0;
        reviveProgress_ = 0.0F;
        downSeconds_ += dt;
        if (beacon_ > 0.0F) {
            beacon_ -= dt;
            if (beacon_ <= 0.0F) {
                beacon_ = 0.0F;
                events_.raise(spawnEvent("beacon"));
            }
        }
        return;
    }

    const std::uint32_t target =
        reviveHeld ? reviveTarget(player_.get<Transform>()->bounds(), hooks_.localPlayerId(), teammates) : 0;
    if (target == 0 || target != reviveTarget_) {
        reviveTarget_ = target;
        reviveProgress_ = 0.0F;
    }
    if (target == 0) {
        return;
    }
    reviveProgress_ += dt;
    if (reviveProgress_ >= reviveSeconds) {
        Event revive(ReviveEvent);
        revive.set("target", static_cast<std::int64_t>(target));
        events_.raise(std::move(revive));
        reviveTarget_ = 0;
        reviveProgress_ = 0.0F;
    }
}

bool StationEvents::canRestart(bool teammateCanHelp) const
{
    return down_ && astronaut_.lives == 0 && (!teammateCanHelp || downSeconds_ >= restartWaitSeconds);
}

void StationEvents::restartAlone()
{
    astronaut_.lives = 3;
    events_.raise(spawnEvent("restart"));
}

CrewFlags StationEvents::flags() const
{
    return {astronaut_.flipped(), down_};
}

void StationEvents::onCollision(const Event& event)
{
    if (event.origin != 0 || down_) {
        return;
    }
    const std::string phase = event.getOr<std::string>("phase", "");
    if (phase != "enter" && phase != "stay") {
        return;
    }
    const auto self = static_cast<std::int64_t>(player_.id());
    const std::int64_t a = event.getOr<std::int64_t>("a", 0);
    const std::int64_t b = event.getOr<std::int64_t>("b", 0);
    if (a != self && b != self) {
        return;
    }
    const std::int64_t other = a == self ? b : a;
    const std::string tag = event.getOr<std::string>(a == self ? "tagB" : "tagA", "");

    // Drones raise "stay" too, so a shield that runs out while still inside
    // a drone does not leave the astronaut safe there. Everything else reacts
    // once, on "enter".
    if (tag == "drone") {
        if (astronaut_.invulnerableSeconds > 0.0F) {
            return;
        }
        Event death(Events::Death);
        death.set("object", self).set("cause", "drone");
        events_.raise(std::move(death));
    } else if (phase != "enter") {
        return;
    } else if (tag == "deathzone") {
        Event death(Events::Death);
        death.set("object", self).set("cause", "breach");
        events_.raise(std::move(death));
    } else if (tag == "cargo") {
        for (std::size_t i = 0; i < station_.cargo.size(); ++i) {
            if (static_cast<std::int64_t>(station_.cargo[i]->id()) == other) {
                hooks_.collectCargo(i);
            }
        }
    } else if (tag == "gravitypanel") {
        // A station-wide gravity failure overrides the panels while it lasts.
        if (gravityAlarms_ == 0 && panelCooldown_ <= 0.0F) {
            panelFlipped_ = !panelFlipped_;
            panelCooldown_ = panelCooldownSeconds;
            applyGravity();
        }
    }
}

void StationEvents::onDeath(const Event& event)
{
    const bool breach = event.getOr<std::string>("cause", "") == "breach";
    if (event.origin != 0) {
        show(breach ? Banner::Kind::Breach : Banner::Kind::Down, event.origin, 0);
        return;
    }
    if (down_) {
        return;
    }
    if (breach) {
        events_.raise(spawnEvent("breach"));
        return;
    }
    astronaut_.lives = std::max(0, astronaut_.lives - 1);
    down_ = true;
    downSeconds_ = 0.0F;
    // With no lives left, only a teammate can bring it back.
    beacon_ = astronaut_.lives > 0 ? beaconSeconds : 0.0F;
    if (Motion* motion = player_.get<Motion>()) {
        motion->velocityX = 0.0F;
        motion->velocityY = 0.0F;
    }
    astronaut_.grounded = false;
    astronaut_.supportId = 0;
}

void StationEvents::onSpawn(const Event& event)
{
    if (event.origin != 0) {
        return;
    }
    panelFlipped_ = false;
    if (event.has("x") && event.has("y")) {
        astronaut_.respawnAt(static_cast<float>(event.getOr<double>("x", 0.0)),
                             static_cast<float>(event.getOr<double>("y", 0.0)));
    } else {
        astronaut_.respawn();
    }
    down_ = false;
    beacon_ = 0.0F;
    astronaut_.invulnerableSeconds = static_cast<float>(event.getOr<double>("shield", 1.0));
    applyGravity();
}

void StationEvents::onRevive(const Event& event)
{
    const auto target = static_cast<std::uint32_t>(event.getOr<std::int64_t>("target", 0));
    if (event.origin == 0) {
        show(Banner::Kind::Revived, target, 0);
        return;
    }
    const std::uint32_t self = hooks_.localPlayerId();
    if (self == 0 || target != self) {
        show(Banner::Kind::Revived, target, event.origin);
        return;
    }
    if (!down_) {
        return;
    }
    // Another player's word is not enough: the rescuer has to be next to this
    // player, so a revive cannot come from across the station.
    Rect rescuer{};
    if (!hooks_.findPlayer || !hooks_.findPlayer(event.origin, rescuer)) {
        return;
    }
    const Rect here = player_.get<Transform>()->bounds();
    const float dx = (rescuer.x + rescuer.width * 0.5F) - (here.x + here.width * 0.5F);
    const float dy = (rescuer.y + rescuer.height * 0.5F) - (here.y + here.height * 0.5F);
    if (std::sqrt(dx * dx + dy * dy) > reviveAcceptRange) {
        return;
    }
    show(Banner::Kind::Revived, 0, event.origin);
    Event spawn = spawnEvent("revive");
    spawn.set("shield", 2.0).set("x", static_cast<double>(rescuer.x)).set("y", static_cast<double>(rescuer.y));
    events_.raise(std::move(spawn));
}

void StationEvents::onAlarm(const Event& event)
{
    // Only this player (as the director) or the current director sets off
    // alarms; anyone else's StationAlarmEvent is ignored.
    if (event.origin != 0 && (!hooks_.alarmDirectorId || event.origin != hooks_.alarmDirectorId())) {
        return;
    }
    const std::string kind = event.getOr<std::string>("kind", "");
    if (kind != "gravity" && kind != "blackout") {
        return;
    }
    const double warning = seconds(event, "warning", 2.0, 0.0, 10.0);
    const double duration = seconds(event, "duration", 6.0, 0.5, 30.0);
    warningKind_ = kind;
    warningSeconds_ = static_cast<float>(warning);

    // Both edges are delayed on game time, so a paused player's alarm waits
    // for them, and a slowed one runs slower. High, like spawns: a dropped
    // end would leave the alarm on for good.
    Event begin(AlarmBeginEvent);
    begin.priority = EventPriority::High;
    begin.set("kind", kind);
    events_.raise(std::move(begin), tics(warning));
    Event end(AlarmEndEvent);
    end.priority = EventPriority::High;
    end.set("kind", kind);
    events_.raise(std::move(end), tics(warning + duration));
}

void StationEvents::onAlarmEdge(const Event& event, int change)
{
    if (event.origin != 0) {
        return;
    }
    const std::string kind = event.getOr<std::string>("kind", "");
    if (kind != "gravity" && kind != "blackout") {
        return;
    }
    if (change > 0 && kind == warningKind_) {
        warningKind_.clear();
        warningSeconds_ = 0.0F;
    }
    // Counted, so two overlapping alarms of one kind end with the later one.
    int& active = kind == "gravity" ? gravityAlarms_ : blackoutAlarms_;
    active = std::max(0, active + change);
    if (kind == "gravity") {
        applyGravity();
    }
}

void StationEvents::applyGravity()
{
    // A gravity failure reverses whatever the panels set.
    astronaut_.setFlipped(panelFlipped_ != (gravityAlarms_ > 0));
}

void StationEvents::show(Banner::Kind kind, std::uint32_t subject, std::uint32_t actor)
{
    banner_ = {kind, subject, actor, bannerSeconds};
}

} // namespace StarfallSalvage
