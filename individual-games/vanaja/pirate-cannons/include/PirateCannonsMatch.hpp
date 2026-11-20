#pragma once

#include "Color.hpp"
#include "Components.hpp"
#include "EventManager.hpp"
#include "FrameTime.hpp"
#include "PirateCannonsRules.hpp"
#include "World.hpp"

#include <array>
#include <cstdint>
#include <random>
#include <vector>

namespace PirateCannons {

// Data only: whose ship this is.
class Ship : public Component {
public:
    explicit Ship(Side side) : side(side) {}

    Side side;
};

// Data only: who fired a cannonball, and whether it splits at its peak.
class Cannonball : public Component {
public:
    Cannonball(Side shooter, bool chain) : shooter(shooter), chain(chain) {}

    Side shooter;
    bool chain;
};

// Removes its object after `seconds` of game time. Splinters, splashes and
// feathers use it.
class Lifetime : public Component {
public:
    Lifetime(World& world, float seconds) : world_(world), remaining_(seconds) {}

    void update(const FrameTime& time) override;

private:
    World& world_;
    float remaining_;
};

// One match of Pirate Cannons, you against the computer: a pooled World and
// the gameplay as handlers on an EventManager. SDL-free, so tests drive it
// with a hand-stepped clock.
//
//   CollisionEvent ball + ship    -> 1 damage; at 0 DeathEvent {object, cause: "sunk"}
//                  ball + island, sea or gull -> the ball is spent (a gull
//                                    is scared off until the next round)
//   DeathEvent     sunk: the round goes to the other side; the match ends
//                  at roundsToWin, otherwise SpawnEvent {kind: "round"}
//                  after roundBreakSeconds of game time
//   SpawnEvent     kind "round": ships repaired, gulls back, wind rolled
//
// A turn ends once no cannonball is left in the air. A ball is deactivated
// the moment it hits something, so a second CollisionEvent about it in the
// same frame is ignored. Delayed spawns carry the match generation, so one
// raised before restart() is dropped.
class Match {
public:
    enum class State { Aiming, CpuAiming, Flying, RoundOver, MatchOver };
    enum class Outcome { None, Hit, Splash, Rocks, Gull, Miss };

    // Subscribes on construction and unsubscribes on destruction, both on the
    // thread that owns `events`, whose clock must count game microseconds.
    Match(EventManager& events, std::uint32_t seed);
    ~Match();

    Match(const Match&) = delete;
    Match& operator=(const Match&) = delete;

    // A new match from round 1. The round begins at the next dispatch.
    void restart();

    // Once per frame on the owning thread, then World::update(). dt is 0
    // while the game is paused.
    void update(const FrameTime& time);

    // Turns the player's cannon, scaled by dt; only while aiming.
    void adjustAim(float angleAxis, float powerAxis, float dt);
    // The player's shot. False when it is not the player's turn, or when
    // `chain` is asked for with none left.
    bool fire(bool chain);

    World& world() { return world_; }
    const World& world() const { return world_; }

    State state() const { return state_; }
    Side turn() const { return turn_; }
    const Shot& shot(Side side) const { return shots_[index(side)]; }
    float wind() const { return wind_; }
    int hp(Side side) const { return hp_[index(side)]; }
    int roundsWon(Side side) const { return roundsWon_[index(side)]; }
    int round() const { return round_; }
    int chainShotsLeft() const { return chainShotsLeft_; }
    int cpuMisses() const { return cpuMisses_; }
    int ballsInFlight() const;
    // The side whose ship is sinking, and how far (0 to 1); only after a round.
    Side sinking() const { return sinking_; }
    float sinkProgress() const { return sinkProgress_; }
    Outcome outcome() const { return outcome_; }
    float outcomeSecondsLeft() const { return outcomeSeconds_; }
    std::uint64_t cosmeticsSkipped() const { return skipped_; }

private:
    void onCollision(const Event& event);
    void onDeath(const Event& event);
    void onSpawn(const Event& event);
    void launch(Side shooter, Shot shot, bool chain);
    void splitChainShots();
    void cullStray();
    void endTurn();
    void cpuTurn(float dt);
    void releaseGulls();
    void show(Outcome outcome);
    void burst(float x, float y, Color color, int count, float speed, float seconds);
    bool spawnBit(float x, float y, float velocityX, float velocityY, Color color, float seconds);
    void remove(GameObject& object);
    float roll() { return unit_(rng_); }

    EventManager& events_;
    World world_{objectCapacity};
    std::vector<EventManager::SubscriptionId> subscriptions_;
    std::mt19937 rng_;
    std::uniform_real_distribution<float> unit_{0.0F, 1.0F};

    // Made once and kept for the whole match.
    std::array<GameObject*, 2> ships_{};

    State state_ = State::Aiming;
    std::uint64_t generation_ = 0;
    Side turn_ = Side::Player;
    std::array<Shot, 2> shots_{};
    std::array<int, 2> hp_{{shipHp, shipHp}};
    std::array<int, 2> roundsWon_{};
    int round_ = 1;
    float wind_ = 0.0F;
    int chainShotsLeft_ = chainShotsPerMatch;
    float cpuTimer_ = 0.0F;
    int cpuMisses_ = 0;
    // Whether the shot in the air has hit the other ship yet.
    bool shotHit_ = false;
    Side sinking_ = Side::Player;
    float sinkProgress_ = 0.0F;
    Outcome outcome_ = Outcome::None;
    float outcomeSeconds_ = 0.0F;
    std::uint64_t skipped_ = 0;
};

} // namespace PirateCannons
