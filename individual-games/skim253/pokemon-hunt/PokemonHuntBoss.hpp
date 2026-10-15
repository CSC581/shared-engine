#pragma once

#include "Components.hpp"
#include "FrameTime.hpp"
#include "Multiplayer.hpp"
#include "PokemonHuntWorld.hpp"
#include "World.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Mewtwo: walks the level, and every few seconds jumps or charges a shadow
// ball. At 0 HP it faints and drops back in bossRespawnSeconds later. Its body
// and balls are GameObjects in its own World; this class sets their intent
// before each World frame. Server only, single thread, SDL-free.
namespace PokemonHunt {

class Boss {
public:
    using Log = std::function<void(const std::string&)>;

    static constexpr float shadowBallLaunchHeight = 70.0F;
    static constexpr float chargeSeconds = 0.7F;
    static constexpr float firstActionDelay = 2.0F;
    static constexpr float minActionDelay = 1.2F;
    static constexpr float maxActionDelay = 2.8F;
    // Each action rolls: jump, charge, or else turn around.
    static constexpr int actionRollSides = 10;
    static constexpr int jumpRolls = 4;
    static constexpr int chargeRolls = 4;
    static constexpr std::uint32_t shadowBallIdRange = 10000;

    explicit Boss(Log log = {}, std::uint32_t seed = std::random_device{}())
        : log_(std::move(log)),
          random_(seed)
    {
        body_ = &world_.create("mewtwo");
        body_->add<Transform>((windowWidth - bossWidth) * 0.5F, groundY - bossHeight, bossWidth, bossHeight);
        motion_ = &body_->add<Motion>();
        gravity_ = &body_->add<Gravity>(0.0F);
        body_->setActive(true);

        // Hidden trigger over the level: a ball that leaves it is destroyed.
        GameObject& level = world_.create("level");
        level.add<Transform>(0.0F, 0.0F, levelWidth, static_cast<float>(windowHeight));
        level.add<Collider>(Collider::Kind::Trigger).onExit = [this](GameObject&, GameObject& other) {
            if (other.tag() == "shadow-ball") {
                world_.destroy(other.id());
            }
        };
        level.setActive(true);
    }

    Boss(const Boss&) = delete;
    Boss& operator=(const Boss&) = delete;

    void update(float dt)
    {
        if (fainted_) {
            updateFainted(dt);
            return;
        }

        motion_->velocityX = state_ == State::Charging ? 0.0F : direction() * bossWalkSpeed;
        gravity_->acceleration = state_ == State::Jumping ? bossGravity : 0.0F;
        gameTimeUs_ += static_cast<std::int64_t>(static_cast<double>(dt) * 1'000'000.0);
        world_.update(FrameTime{static_cast<double>(dt), gameTimeUs_});

        if (state_ == State::Charging) {
            updateCharging(dt);
        } else {
            stayOnScreen();
            if (state_ == State::Jumping) {
                land();
            } else {
                updateWalking(dt);
            }
        }
    }

    // `damage` is a running total; only what is new comes off the HP.
    void reportDamage(std::uint64_t player, long long damage)
    {
        long long& last = reported_[player];
        // A lower total is a new Pokemon reusing a departed one's id.
        if (damage < last) {
            last = damage;
            return;
        }
        const long long fresh = damage - last;
        if (fresh == 0) {
            return;
        }
        last = damage;
        if (fainted_) {
            return;
        }
        hp_ -= static_cast<int>(std::min<long long>(fresh, hp_));
        if (hp_ <= 0) {
            faint();
        }
    }

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

    float x() const { return body_->get<Transform>()->x; }
    float y() const { return body_->get<Transform>()->y; }
    int hp() const { return hp_; }
    bool fainted() const { return fainted_; }
    std::size_t shadowBallCount() { return world_.findAllByTag("shadow-ball").size(); }

    // For tests.
    void jump() { startJump(); }
    void fireShadowBall() { fire(); }
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
    enum class State { Walking, Jumping, Charging };

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

    void updateFainted(float dt)
    {
        respawnIn_ -= dt;
        if (respawnIn_ <= 0.0F) {
            respawn();
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

    void faint()
    {
        fainted_ = true;
        respawnIn_ = bossRespawnSeconds;
        ++knockouts_;
        body_->setActive(false);
        for (GameObject* ball : world_.findAllByTag("shadow-ball")) {
            world_.destroy(ball->id());
        }
        world_.flushDestroyed();
        say("Mewtwo fainted (KO " + std::to_string(knockouts_) + "); back in " +
            std::to_string(static_cast<int>(bossRespawnSeconds)) + "s");
    }

    void respawn()
    {
        fainted_ = false;
        hp_ = bossMaxHp;
        Transform& box = body();
        box.x = std::uniform_real_distribution<float>(bossMinX, bossMaxX)(random_);
        box.y = -bossHeight;
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
        return result;
    }

    Log log_;
    std::mt19937 random_;
    World world_;
    GameObject* body_ = nullptr;
    Motion* motion_ = nullptr;
    Gravity* gravity_ = nullptr;
    std::int64_t gameTimeUs_ = 0;
    State state_ = State::Walking;
    bool facingLeft_ = true;
    float timer_ = firstActionDelay;
    std::uint32_t nextBallId_ = shadowBallIdBase;
    int hp_ = bossMaxHp;
    bool fainted_ = false;
    float respawnIn_ = 0.0F;
    long long knockouts_ = 0;
    std::unordered_map<std::uint64_t, long long> reported_;
};

} // namespace PokemonHunt
