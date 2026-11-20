#include "PirateCannonsMatch.hpp"

#include "TimeUnits.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace PirateCannons {

namespace {

constexpr Color ballColor{30, 30, 35};
constexpr Color chainBallColor{70, 70, 90};
constexpr Color woodColor{150, 100, 60};
constexpr Color rockColor{140, 132, 120};
constexpr Color spray{205, 232, 255};
constexpr Color featherColor{245, 245, 245};
constexpr float outcomeSeconds = 1.2F;
constexpr int sinkingBits = 90;
constexpr float twoPi = 6.2831853F;

std::int64_t ticsFor(float seconds)
{
    return static_cast<std::int64_t>(static_cast<double>(seconds) * kGameTicsPerSecond);
}

// The gulls' flight paths: two back and forth, two circling over the island.
struct GullPath {
    bool circular;
    PathMover::Linear linear;
    PathMover::Circular circle;
    float speed;
};

constexpr GullPath gullPaths[gulls] = {
    {false, {300.0F, 170.0F, 640.0F, 170.0F}, {}, 90.0F},
    {false, {650.0F, 235.0F, 310.0F, 235.0F}, {}, 70.0F},
    {true, {}, {455.0F, 190.0F, 55.0F}, 80.0F},
    {true, {}, {470.0F, 110.0F, 80.0F}, 60.0F},
};

} // namespace

Match::Match(EventManager& events, std::uint32_t seed)
    : events_(events), rng_(seed)
{
    world_.setEventManager(&events_);

    // Ships first: a ball touching a ship and the sea in one frame reports
    // the ship first, so a hit at the waterline counts.
    for (const Side side : {Side::Player, Side::Cpu}) {
        GameObject& ship = world_.create("ship");
        const Rect bounds = shipBounds(side);
        ship.add<Transform>(bounds.x, bounds.y, bounds.width, bounds.height);
        ship.add<Ship>(side);
        ship.add<Collider>(Collider::Kind::Trigger, shipLayer);
        ship.setActive(true);
        ships_[index(side)] = &ship;
    }

    // The island and the sea are hidden triggers; the game draws them.
    GameObject& island = world_.create("island");
    island.add<Transform>(islandX, islandY, islandWidth, islandHeight);
    island.add<Collider>(Collider::Kind::Trigger, islandLayer);
    island.setActive(true);

    GameObject& sea = world_.create("sea");
    sea.add<Transform>(0.0F, seaY, static_cast<float>(width), height - seaY);
    sea.add<Collider>(Collider::Kind::Trigger, seaLayer);
    sea.setActive(true);

    subscriptions_.push_back(events_.subscribe(Events::Collision, [this](const Event& event) { onCollision(event); }));
    subscriptions_.push_back(events_.subscribe(Events::Death, [this](const Event& event) { onDeath(event); }));
    subscriptions_.push_back(events_.subscribe(Events::Spawn, [this](const Event& event) { onSpawn(event); }));
    restart();
}

Match::~Match()
{
    for (const EventManager::SubscriptionId id : subscriptions_) {
        events_.unsubscribe(id);
    }
}

void Match::restart()
{
    ++generation_;
    for (const auto& object : world_.objects()) {
        const std::string& tag = object->tag();
        if (tag != "ship" && tag != "island" && tag != "sea") {
            world_.destroy(object->id());
        }
    }
    // Frees the old match's slots now, before the new round needs them.
    world_.flushDestroyed();

    state_ = State::RoundOver;
    hp_ = {{shipHp, shipHp}};
    roundsWon_ = {{0, 0}};
    chainShotsLeft_ = chainShotsPerMatch;
    outcome_ = Outcome::None;
    outcomeSeconds_ = 0.0F;
    skipped_ = 0;

    Event round(Events::Spawn);
    round.set("kind", "round").set("round", 1).set("generation", static_cast<std::int64_t>(generation_));
    round.priority = EventPriority::High;
    events_.raise(std::move(round));
}

void Match::update(const FrameTime& time)
{
    const float dt = static_cast<float>(time.dtSeconds);
    if (dt > 0.0F) {
        outcomeSeconds_ = std::max(0.0F, outcomeSeconds_ - dt);
        if (state_ == State::CpuAiming) {
            cpuTurn(dt);
        }
        if ((state_ == State::RoundOver || state_ == State::MatchOver) && hp(sinking_) == 0) {
            sinkProgress_ = std::min(1.0F, sinkProgress_ + dt / sinkSeconds);
        }
    }
    splitChainShots();
    world_.update(time);
    cullStray();
    if (state_ == State::Flying && ballsInFlight() == 0) {
        endTurn();
    }
}

void Match::adjustAim(float angleAxis, float powerAxis, float dt)
{
    if (state_ != State::Aiming) {
        return;
    }
    Shot& shot = shots_[index(Side::Player)];
    shot = clampShot({shot.angle + angleAxis * angleRate * dt, shot.power + powerAxis * powerRate * dt});
}

bool Match::fire(bool chain)
{
    if (state_ != State::Aiming || turn_ != Side::Player || (chain && chainShotsLeft_ <= 0)) {
        return false;
    }
    if (chain) {
        --chainShotsLeft_;
    }
    launch(Side::Player, shots_[index(Side::Player)], chain);
    return true;
}

int Match::ballsInFlight() const
{
    int count = 0;
    for (const auto& object : world_.objects()) {
        if (object->isActive() && object->tag() == "ball") {
            ++count;
        }
    }
    return count;
}

void Match::onCollision(const Event& event)
{
    if (event.origin != 0 || event.getOr<std::string>("phase", "") != "enter") {
        return;
    }
    GameObject* a = world_.find(static_cast<ObjectId>(event.getOr<std::int64_t>("a", 0)));
    GameObject* b = world_.find(static_cast<ObjectId>(event.getOr<std::int64_t>("b", 0)));
    if (a == nullptr || b == nullptr || !a->isActive() || !b->isActive()) {
        return;
    }
    if (b->tag() == "ball") {
        std::swap(a, b);
    }
    // Chain-shot balls can overlap each other; only what they hit matters.
    if (a->tag() != "ball" || b->tag() == "ball") {
        return;
    }
    GameObject& ball = *a;
    GameObject& target = *b;
    const Transform& at = *ball.get<Transform>();
    const float x = at.x + ballSize * 0.5F;
    const float y = at.y + ballSize * 0.5F;
    const Side shooter = ball.get<Cannonball>()->shooter;
    remove(ball);

    const std::string& tag = target.tag();
    if (tag == "ship") {
        const Side side = target.get<Ship>()->side;
        if (hp_[index(side)] == 0) {
            return;
        }
        --hp_[index(side)];
        if (side != shooter) {
            shotHit_ = true;
        }
        burst(x, y, woodColor, 12, 160.0F, 1.0F);
        show(Outcome::Hit);
        if (hp_[index(side)] == 0) {
            Event death(Events::Death);
            death.set("object", target.id()).set("cause", "sunk");
            death.priority = EventPriority::High;
            events_.raise(std::move(death));
        }
    } else if (tag == "island") {
        burst(x, y, rockColor, 8, 120.0F, 0.8F);
        show(Outcome::Rocks);
    } else if (tag == "sea") {
        burst(x, seaY, spray, 12, 140.0F, 0.9F);
        show(Outcome::Splash);
    } else if (tag == "gull") {
        // Scared off until the next round.
        remove(target);
        burst(x, y, featherColor, 8, 90.0F, 1.2F);
        show(Outcome::Gull);
    }
}

void Match::onDeath(const Event& event)
{
    if (event.origin != 0 || event.getOr<std::string>("cause", "") != "sunk") {
        return;
    }
    GameObject* object = world_.find(static_cast<ObjectId>(event.getOr<std::int64_t>("object", 0)));
    const Ship* ship = object != nullptr ? object->get<Ship>() : nullptr;
    if (ship == nullptr || state_ == State::RoundOver || state_ == State::MatchOver) {
        return;
    }
    const Side loser = ship->side;
    const Side winner = other(loser);
    // Sinking: still drawn, no longer a target.
    object->get<Collider>()->layers = 0U;
    hp_[index(loser)] = 0;
    sinking_ = loser;
    sinkProgress_ = 0.0F;
    ++roundsWon_[index(winner)];
    for (const auto& leftover : world_.objects()) {
        if (leftover->isActive() && leftover->tag() == "ball") {
            remove(*leftover);
        }
    }
    // More splinters than the cosmetic budget on purpose, so the pool counter
    // shows the skipping.
    const Transform& hull = *object->get<Transform>();
    burst(hull.x + shipWidth * 0.5F, hull.y + shipHeight * 0.5F, woodColor, sinkingBits, 220.0F, 1.6F);

    if (roundsWon_[index(winner)] >= roundsToWin) {
        state_ = State::MatchOver;
        return;
    }
    state_ = State::RoundOver;
    Event round(Events::Spawn);
    round.set("kind", "round").set("round", round_ + 1).set("generation", static_cast<std::int64_t>(generation_));
    round.priority = EventPriority::High;
    events_.raise(std::move(round), ticsFor(roundBreakSeconds));
}

void Match::onSpawn(const Event& event)
{
    if (event.origin != 0 || event.getOr<std::string>("kind", "") != "round" ||
        event.getOr<std::int64_t>("generation", -1) != static_cast<std::int64_t>(generation_)) {
        return;
    }
    round_ = static_cast<int>(event.getOr<std::int64_t>("round", 1));
    hp_ = {{shipHp, shipHp}};
    sinkProgress_ = 0.0F;
    for (GameObject* ship : ships_) {
        ship->get<Collider>()->layers = shipLayer;
    }
    for (const auto& object : world_.objects()) {
        if (object->isActive() && object->tag() == "ball") {
            remove(*object);
        }
    }
    releaseGulls();

    // Rounds alternate who fires first.
    turn_ = round_ % 2 == 1 ? Side::Player : Side::Cpu;
    cpuMisses_ = 0;
    shotHit_ = false;
    wind_ = rollWind(roll());
    state_ = turn_ == Side::Player ? State::Aiming : State::CpuAiming;
    cpuTimer_ = cpuThinkSeconds;
}

void Match::launch(Side shooter, Shot shot, bool chain)
{
    makeBall(shooter, muzzleX(shooter), muzzleY(), launchVelocity(shot, shooter), chain,
             chain ? chainBallColor : ballColor);
    state_ = State::Flying;
    shotHit_ = false;
}

void Match::makeBall(Side shooter, float x, float y, Velocity velocity, bool chain, Color color)
{
    GameObject& ball = world_.create("ball");
    ball.add<Transform>(x, y, ballSize, ballSize);
    Motion& motion = ball.add<Motion>(velocity.x, velocity.y);
    ball.add<Gravity>(gravity);
    // Wind first each frame, then Gravity, then Motion: the order simulate()
    // assumes, so the computer's aim matches the real flight.
    ball.add<Behavior>([this, &motion](GameObject&, const FrameTime& time) {
        motion.velocityX += wind_ * static_cast<float>(time.dtSeconds);
    });
    ball.add<Collider>(Collider::Kind::Trigger, ballLayers);
    ball.add<Renderable>(color).layer = 5;
    ball.add<Cannonball>(shooter, chain);
    ball.setActive(true);
}

// A chain-shot ball splits into three once it starts to come down.
void Match::splitChainShots()
{
    std::vector<GameObject*> splitting;
    for (const auto& object : world_.objects()) {
        const Cannonball* ball = object->get<Cannonball>();
        if (object->isActive() && ball != nullptr && ball->chain && object->get<Motion>()->velocityY >= 0.0F) {
            splitting.push_back(object.get());
        }
    }
    for (GameObject* original : splitting) {
        const Transform& at = *original->get<Transform>();
        const Motion& motion = *original->get<Motion>();
        const Side shooter = original->get<Cannonball>()->shooter;
        remove(*original);
        for (const float factor : {1.0F - chainSpread, 1.0F, 1.0F + chainSpread}) {
            makeBall(shooter, at.x, at.y, {motion.velocityX * factor, motion.velocityY}, false, chainBallColor);
        }
    }
}

void Match::cullStray()
{
    for (const auto& object : world_.objects()) {
        if (!object->isActive() || object->tag() != "ball") {
            continue;
        }
        const Transform& at = *object->get<Transform>();
        if (at.x < -60.0F || at.x > width + 60.0F || at.y > height + 20.0F) {
            remove(*object);
            show(Outcome::Miss);
        }
    }
}

void Match::endTurn()
{
    if (turn_ == Side::Cpu && !shotHit_) {
        ++cpuMisses_;
    }
    turn_ = other(turn_);
    wind_ = rollWind(roll());
    state_ = turn_ == Side::Player ? State::Aiming : State::CpuAiming;
    cpuTimer_ = cpuThinkSeconds;
}

void Match::cpuTurn(float dt)
{
    cpuTimer_ -= dt;
    if (cpuTimer_ > 0.0F) {
        return;
    }
    // Aims at a point beside your ship, closer with every miss.
    const float error = aimError(cpuMisses_);
    const float offset = error * (0.5F + 0.5F * roll()) * (roll() < 0.5F ? -1.0F : 1.0F);
    const Rect target = shipBounds(Side::Player);
    shots_[index(Side::Cpu)] = aimAt(Side::Cpu, target.x + target.width * 0.5F + offset, wind_);
    launch(Side::Cpu, shots_[index(Side::Cpu)], false);
}

void Match::releaseGulls()
{
    for (const auto& object : world_.objects()) {
        if (object->isActive() && object->tag() == "gull") {
            remove(*object);
        }
    }
    for (const GullPath& path : gullPaths) {
        GameObject& gull = world_.create("gull");
        // Placed where its path begins, so PathMover's first step is small.
        const float x = path.circular ? path.circle.centerX + path.circle.radius : path.linear.startX;
        const float y = path.circular ? path.circle.centerY : path.linear.startY;
        gull.add<Transform>(x, y, 22.0F, 12.0F);
        if (path.circular) {
            gull.add<PathMover>(path.circle, path.speed);
        } else {
            gull.add<PathMover>(path.linear, path.speed);
        }
        gull.add<Collider>(Collider::Kind::Trigger, gullLayer);
        gull.setActive(true);
    }
}

void Match::show(Outcome outcome)
{
    outcome_ = outcome;
    outcomeSeconds_ = outcomeSeconds;
}

void Match::burst(float x, float y, Color color, int count, float speed, float seconds)
{
    for (int i = 0; i < count; ++i) {
        const float angle = twoPi * roll();
        const float launch = speed * (0.4F + 0.6F * roll());
        if (!spawnBit(x, y, std::cos(angle) * launch, std::sin(angle) * launch - speed * 0.6F, color,
                      seconds * (0.6F + 0.4F * roll()))) {
            skipped_ += static_cast<std::uint64_t>(count - i - 1);
            return;
        }
    }
}

bool Match::spawnBit(float x, float y, float velocityX, float velocityY, Color color, float seconds)
{
    GameObject* bit = Arcade::cosmeticRoom(world_, gameplayReserve) ? world_.tryCreate("bit") : nullptr;
    if (bit == nullptr) {
        ++skipped_;
        return false;
    }
    bit->add<Transform>(x - 2.0F, y - 2.0F, 4.0F, 4.0F);
    bit->add<Motion>(velocityX, velocityY);
    bit->add<Gravity>(gravity);
    bit->add<Renderable>(color).layer = 6;
    bit->add<Arcade::Lifetime>(world_, seconds);
    bit->setActive(true);
    return true;
}

void Match::remove(GameObject& object)
{
    object.setActive(false);
    world_.destroy(object.id());
}

} // namespace PirateCannons
