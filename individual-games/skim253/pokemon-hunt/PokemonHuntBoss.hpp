#pragma once

#include "Components.hpp"
#include "Event.hpp"
#include "EventManager.hpp"
#include "FrameTime.hpp"
#include "Multiplayer.hpp"
#include "PokemonHuntEvents.hpp"
#include "PokemonHuntWorld.hpp"
#include "TimeSource.hpp"
#include "TimeUnits.hpp"
#include "World.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

// Server-side Mewtwo AI with a private World and EventManager.
// Updated on one thread; no SDL dependencies.
namespace PokemonHunt {

class Boss {
public:
    using Log = std::function<void(const std::string&)>;
    // Returns the player's top-left position, or false for an unknown player.
    using PlayerLocator = std::function<bool(std::uint32_t player, float& x, float& y)>;

    // Per-player damage budget, allowing bursts and 2x Haste.
    static constexpr float hitBudgetPerSecond = 12.0F;
    static constexpr float hitBudgetBurst = 9.0F;
    // How far a hit may be from where Mewtwo sees things, for network delay.
    static constexpr float hitSlackX = 120.0F;
    static constexpr float hitSlackY = 160.0F;
    // Approximate shot range plus latency allowance.
    static constexpr float shotRange = static_cast<float>(windowWidth) + hitSlackX;

    static constexpr float shadowBallLaunchHeight = 70.0F;
    static constexpr float chargeSeconds = 0.7F;
    static constexpr float firstActionDelay = 2.0F;
    static constexpr float minActionDelay = 1.2F;
    static constexpr float maxActionDelay = 2.8F;
    // Non-Psystrike action weights: jump, shadow-ball charge, then turn.
    static constexpr int actionRollSides = 10;
    static constexpr int jumpRolls = 4;
    static constexpr int chargeRolls = 4;
    // Psystrike chance per eligible action, as a percentage.
    static constexpr int psystrikePercent = 35;
    static constexpr std::uint32_t shadowBallIdRange = 10000;

    explicit Boss(Log log = {}, std::uint32_t seed = std::random_device{}())
        : log_(std::move(log)),
          random_(seed),
          events_(clock_)
    {
        world_.setEventManager(&events_);
        body_ = &world_.create("mewtwo");
        body_->add<Transform>((windowWidth - bossWidth) * 0.5F, groundY - bossHeight, bossWidth, bossHeight);
        motion_ = &body_->add<Motion>();
        gravity_ = &body_->add<Gravity>(0.0F);
        body_->setActive(true);

        // Hidden trigger over the level: a ball leaving it raises an "exit".
        level_ = &world_.create("level");
        level_->add<Transform>(0.0F, 0.0F, levelWidth, static_cast<float>(windowHeight));
        level_->add<Collider>(Collider::Kind::Trigger);
        level_->setActive(true);

        events_.subscribe(Events::Collision, [this](const Event& event) { onCollision(event); });
        events_.subscribe(GameEvents::Hit, [this](const Event& event) { onHit(event); });
        events_.subscribe(Events::Death, [this](const Event& event) { onDeath(event); });
        events_.subscribe(Events::Spawn, [this](const Event& event) { onSpawn(event); });
        events_.subscribe(GameEvents::PsystrikeDue, [this](const Event& event) { onPsystrikeDue(event); });
        events_.subscribe(GameEvents::ClockPing, [this](const Event& event) { onClockPing(event); });
    }

    Boss(const Boss&) = delete;
    Boss& operator=(const Boss&) = delete;
    void update(float dt)
    {
        clock_.us += static_cast<std::int64_t>(static_cast<double>(dt) * kGameTicsPerSecond);
        if (fainted_) {
            updateFainted(dt);
        } else {
            move(dt);
        }
        events_.dispatch();
    }

    // Boss time is in microseconds; the network bridge delivers hits here.
    EventManager& events() { return events_; }

    // Without a locator (tests), hits are not checked against positions.
    void setPlayerLocator(PlayerLocator locator) { locate_ = std::move(locator); }
    std::uint64_t refusedHits() const { return refusedHits_; }
    std::int64_t clockUs() const { return clock_.us; }

    std::vector<Multiplayer::Platform> objects()
    {
        std::vector<Multiplayer::Platform> result;
        result.push_back(statusObject());
        if (fainted_) {
            return result;
        }
        result.push_back(toPlatform(bossIdBase + flags(), *body_->get<Transform>()));
        for (GameObject* ball : world_.findAllByTag("shadow-ball")) {
            result.push_back(toPlatform(ball->get<NetworkIdentity>()->id, *ball->get<Transform>()));
        }
        return result;
    }

    ObjectId id() const { return body_->id(); }
    float x() const { return body_->get<Transform>()->x; }
    float y() const { return body_->get<Transform>()->y; }
    int hp() const { return hp_; }
    bool chargingPsystrike() const { return state_ == State::Psystrike; }
    bool fainted() const { return fainted_; }
    std::size_t shadowBallCount() { return world_.findAllByTag("shadow-ball").size(); }

    // For tests.
    void jump() { startJump(); }
    void fireShadowBall() { fire(); }
    void psystrike() { startPsystrike(); }
    void walkFrom(float x, bool faceLeft)
    {
        body().x = x;
        body().y = groundY - bossHeight;
        motion_->velocityY = 0.0F;
        facingLeft_ = faceLeft;
        state_ = State::Walking;
        timer_ = 60.0F;
    }

private:
    enum class State { Walking, Jumping, Charging, Psystrike };

    struct Budget {
        float tokens;
        std::int64_t refilledAt;
    };

    // Mewtwo's game time for its EventManager, advanced by update().
    struct GameClock final : TimeSource {
        std::int64_t now() const override { return us; }
        std::int64_t us = 0;
    };

    static Multiplayer::Platform toPlatform(std::uint32_t id, const Transform& box)
    {
        return {id, box.x, box.y, box.width, box.height};
    }

    float direction() const { return facingLeft_ ? -1.0F : 1.0F; }

    Transform& body() { return *body_->get<Transform>(); }

    void say(const std::string& line) const
    {
        if (log_) {
            log_(line);
        }
    }

    void move(float dt)
    {
        const bool standing = state_ == State::Charging || state_ == State::Psystrike;
        motion_->velocityX = standing ? 0.0F : direction() * bossWalkSpeed;
        gravity_->acceleration = state_ == State::Jumping ? bossGravity : 0.0F;
        psystrikeCooldown_ = std::max(0.0F, psystrikeCooldown_ - dt);
        world_.update(FrameTime{static_cast<double>(dt), clock_.us});

        if (standing) {
            if (state_ == State::Charging) {
                updateCharging(dt);
            }
            return; // Psystrike ends through its delayed event.
        }
        stayOnScreen();
        if (state_ == State::Jumping) {
            land();
        } else {
            updateWalking(dt);
        }
    }

    void updateFainted(float dt)
    {
        respawnIn_ -= dt;
        if (respawnIn_ <= 0.0F && !respawnRequested_) {
            respawnRequested_ = true;
            const float x = std::uniform_real_distribution<float>(bossMinX, bossMaxX)(random_);
            events_.raise(respawnEvent(id(), x, -bossHeight));
        }
    }

    void onCollision(const Event& event)
    {
        Contact contact;
        if (contactOf(event, level_->id(), contact) && contact.phase == "exit" &&
            contact.otherTag == "shadow-ball") {
            world_.destroy(contact.other);
        }
    }

    // Accept remote hits only after kind, position and rate checks.
    void onHit(const Event& event)
    {
        if (event.origin == 0 || fainted_ || hp_ <= 0) {
            return;
        }
        const std::string kind = event.getOr<std::string>("kind", "");
        const int damage = damageFor(kind);
        const char* problem = damage == 0                           ? "unknown kind"
                              : !plausibleFrom(event.origin, kind) ? "not where it could hit"
                              : !spendBudget(event.origin, damage) ? "too many hits"
                                                                   : nullptr;
        if (problem != nullptr) {
            refuse(event.origin, problem);
            return;
        }
        hp_ -= std::min(damage, hp_);
        if (hp_ <= 0) {
            events_.raise(deathEvent(id(), "knockout").set("finisher", static_cast<std::int64_t>(event.origin)));
        }
    }

    // Check reported player positions with generous latency tolerances.
    bool plausibleFrom(std::uint32_t player, const std::string& kind) const
    {
        if (!locate_) {
            return true;
        }
        float px = 0.0F;
        float py = 0.0F;
        if (!locate_(player, px, py)) {
            return false;
        }
        const Transform& box = *body_->get<Transform>();
        const float apartX = std::fabs((px + pokemonSize * 0.5F) - (box.x + box.width * 0.5F));
        if (kind == GameEvents::HitStomp) {
            const float feet = py + pokemonSize;
            return apartX <= (box.width + pokemonSize) * 0.5F + hitSlackX && feet >= box.y - hitSlackY &&
                   feet <= box.y + box.height * 0.5F;
        }
        const float shotY = py + pokemonSize * shotSpawnHeightRatio;
        return apartX <= shotRange && shotY >= box.y - hitSlackY && shotY <= box.y + box.height + hitSlackY;
    }

    // A token bucket per player, refilled on Mewtwo's clock.
    bool spendBudget(std::uint32_t player, int damage)
    {
        auto [entry, isNew] = budgets_.try_emplace(player, Budget{hitBudgetBurst, clock_.us});
        Budget& budget = entry->second;
        if (!isNew) {
            const float seconds = static_cast<float>(clock_.us - budget.refilledAt) / static_cast<float>(kGameTicsPerSecond);
            budget.tokens = std::min(hitBudgetBurst, budget.tokens + seconds * hitBudgetPerSecond);
            budget.refilledAt = clock_.us;
        }
        if (budget.tokens < static_cast<float>(damage)) {
            return false;
        }
        budget.tokens -= static_cast<float>(damage);
        return true;
    }

    // Logged on the first refusal and every tenth after, so a flood stays quiet.
    void refuse(std::uint32_t player, const char* problem)
    {
        if (refusedHits_++ % 10 == 0) {
            say("Mewtwo refused a hit from player " + std::to_string(player) + ": " + problem + " (" +
                std::to_string(refusedHits_) + " refused so far)");
        }
    }

    void onClockPing(const Event& event)
    {
        if (event.origin == 0) {
            return;
        }
        events_.raise(Event{GameEvents::ClockPong}
                          .set("player", static_cast<std::int64_t>(event.origin))
                          .set("sentAt", event.getOr<std::int64_t>("sentAt", 0))
                          .set("bossTime", clock_.us));
    }

    // A Pokemon's DeathEvent (it fell in a pit) also arrives here; skip it.
    void onDeath(const Event& event)
    {
        if (isLocalAbout(event, id())) {
            faint(event.getOr<std::int64_t>("finisher", 0));
        }
    }

    // Attack IDs invalidate delayed strikes after a faint or a replacement charge.
    void startPsystrike()
    {
        state_ = State::Psystrike;
        ++attack_;
        const double centre = static_cast<double>(x() + bossWidth * 0.5F);
        events_.raise(Event{GameEvents::Psystrike}
                          .set("stage", "charge")
                          .set("x", centre)
                          .set("seconds", static_cast<double>(psystrikeChargeSeconds))
                          .set("at", clock_.us + chargeUs()));
        events_.raise(Event{GameEvents::PsystrikeDue}.set("attack", attack_), chargeUs());
        say("Mewtwo charges Psystrike");
    }

    static std::int64_t chargeUs()
    {
        return static_cast<std::int64_t>(static_cast<double>(psystrikeChargeSeconds) * kGameTicsPerSecond);
    }

    void onPsystrikeDue(const Event& event)
    {
        if (event.origin != 0 || event.get<std::int64_t>("attack") != attack_ || state_ != State::Psystrike ||
            fainted_) {
            return;
        }
        // High prevents local queue-limit drops; network delivery remains best effort.
        Event strike{GameEvents::Psystrike};
        strike.priority = EventPriority::High;
        strike.set("stage", "strike").set("x", static_cast<double>(x() + bossWidth * 0.5F)).set("at", clock_.us);
        events_.raise(std::move(strike));
        state_ = State::Walking;
        timer_ = nextActionDelay();
        psystrikeCooldown_ = psystrikeCooldownSeconds;
    }

    void onSpawn(const Event& event)
    {
        if (isLocalAbout(event, id())) {
            respawn(static_cast<float>(event.get<double>("x")), static_cast<float>(event.get<double>("y")));
        }
    }

    void updateCharging(float dt)
    {
        timer_ -= dt;
        if (timer_ <= 0.0F) {
            fire();
            state_ = State::Walking;
            timer_ = nextActionDelay();
        }
    }

    void stayOnScreen()
    {
        Transform& box = body();
        if (box.x <= bossMinX) {
            box.x = bossMinX;
            facingLeft_ = false;
        } else if (box.x >= bossMaxX) {
            box.x = bossMaxX;
            facingLeft_ = true;
        }
    }

    void land()
    {
        Transform& box = body();
        if (box.y >= groundY - bossHeight) {
            box.y = groundY - bossHeight;
            motion_->velocityY = 0.0F;
            state_ = State::Walking;
        }
    }

    void updateWalking(float dt)
    {
        timer_ -= dt;
        if (timer_ <= 0.0F) {
            startAction();
        }
    }

    void fire()
    {
        const Transform& box = body();
        const float startX = facingLeft_ ? box.x - shadowBallSize : box.x + bossWidth;
        GameObject& ball = world_.create("shadow-ball");
        ball.add<Transform>(startX, groundY - shadowBallLaunchHeight, shadowBallSize, shadowBallSize);
        ball.add<Motion>(direction() * shadowBallSpeed, 0.0F);
        ball.add<NetworkIdentity>(NetworkIdentity::Kind::Platform, nextBallId_++);
        ball.add<Collider>(Collider::Kind::Trigger);
        ball.setActive(true);
        if (nextBallId_ > shadowBallIdBase + shadowBallIdRange) {
            nextBallId_ = shadowBallIdBase;
        }
    }

    void startJump()
    {
        state_ = State::Jumping;
        motion_->velocityY = -bossJumpSpeed;
    }

    void startAction()
    {
        if (psystrikeCooldown_ <= 0.0F && std::uniform_int_distribution<int>(0, 99)(random_) < psystrikePercent) {
            startPsystrike();
            return;
        }
        const int roll = std::uniform_int_distribution<int>(0, actionRollSides - 1)(random_);
        if (roll < jumpRolls) {
            startJump();
        } else if (roll < jumpRolls + chargeRolls) {
            state_ = State::Charging;
            timer_ = chargeSeconds;
        } else {
            facingLeft_ = !facingLeft_;
            timer_ = nextActionDelay();
        }
    }

    float nextActionDelay()
    {
        return std::uniform_real_distribution<float>(minActionDelay, maxActionDelay)(random_);
    }

    void faint(std::int64_t finisher)
    {
        fainted_ = true;
        respawnRequested_ = false;
        respawnIn_ = bossRespawnSeconds;
        ++knockouts_;
        body_->setActive(false);
        for (GameObject* ball : world_.findAllByTag("shadow-ball")) {
            world_.destroy(ball->id());
        }
        world_.flushDestroyed();
        say("Mewtwo fainted (KO " + std::to_string(knockouts_) + ", finished by player " +
            std::to_string(finisher) + "); back in " + std::to_string(static_cast<int>(bossRespawnSeconds)) + "s");
    }

    void respawn(float x, float y)
    {
        fainted_ = false;
        hp_ = bossMaxHp;
        Transform& box = body();
        box.x = std::clamp(x, bossMinX, bossMaxX);
        box.y = y;
        motion_->velocityX = 0.0F;
        motion_->velocityY = 0.0F;
        body_->setActive(true);
        facingLeft_ = std::uniform_int_distribution<int>(0, 1)(random_) == 0;
        state_ = State::Jumping;
        timer_ = nextActionDelay();
        say("Mewtwo is back at x=" + std::to_string(static_cast<int>(box.x)));
    }

    Multiplayer::Platform statusObject() const
    {
        const float respawnIn = fainted_ ? std::max(respawnIn_, 0.0F) : 0.0F;
        return {bossStatusId, static_cast<float>(hp_), respawnIn, static_cast<float>(knockouts_ + 1), 1.0F};
    }

    std::uint32_t flags() const
    {
        std::uint32_t result = 0;
        if (facingLeft_) {
            result |= bossFacingLeft;
        }
        if (state_ == State::Charging) {
            result |= bossCharging;
        }
        if (state_ == State::Psystrike) {
            result |= bossPsystrike;
        }
        return result;
    }

    Log log_;
    std::mt19937 random_;
    GameClock clock_;
    // Before world_, which raises into it and so must be destroyed first.
    EventManager events_;
    World world_;
    GameObject* body_ = nullptr;
    GameObject* level_ = nullptr;
    Motion* motion_ = nullptr;
    Gravity* gravity_ = nullptr;
    State state_ = State::Walking;
    bool facingLeft_ = true;
    float timer_ = firstActionDelay;
    std::uint32_t nextBallId_ = shadowBallIdBase;
    int hp_ = bossMaxHp;
    bool fainted_ = false;
    float respawnIn_ = 0.0F;
    bool respawnRequested_ = false;
    float psystrikeCooldown_ = psystrikeFirstDelaySeconds;
    std::int64_t attack_ = 0;
    PlayerLocator locate_;
    std::map<std::uint32_t, Budget> budgets_;
    std::uint64_t refusedHits_ = 0;
    long long knockouts_ = 0;
};

// Read poses from the same session that delivers hits.
inline Boss::PlayerLocator sessionLocator(const Multiplayer::Session& session)
{
    return [&session](std::uint32_t player, float& x, float& y) {
        for (const Multiplayer::Player& other : session.remotePlayers()) {
            if (other.id == player) {
                x = other.x;
                y = other.y;
                return true;
            }
        }
        return false;
    };
}

} // namespace PokemonHunt
