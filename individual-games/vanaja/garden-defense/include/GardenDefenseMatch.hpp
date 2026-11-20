#pragma once

#include "ArcadeParts.hpp"
#include "Color.hpp"
#include "Components.hpp"
#include "EventManager.hpp"
#include "FrameTime.hpp"
#include "GardenDefenseRules.hpp"
#include "World.hpp"

#include <cstdint>
#include <random>
#include <vector>

namespace GardenDefense {

// Data only: where a pest marches, and so what it is worth and which column
// it drops seeds for.
class Pest : public Component {
public:
    Pest(int row, int column) : row(row), column(column) {}

    int row;
    int column;
};

// One match of Garden Defense: a pooled World and the gameplay as handlers
// on an EventManager. SDL-free, so tests drive it with a hand-stepped clock.
//
//   CollisionEvent drop + pest or ladybug -> DeathEvent {object, cause: "sprayed"}
//                  seed + can             -> blocked by the umbrella or the
//                                            respawn shield, otherwise
//                                            DeathEvent {object: can, cause: "seed"}
//                  pest + fence           -> DeathEvent {object: can, cause: "overrun"}
//   DeathEvent     sprayed: points and splats; the last pest clears the wave
//                  seed: a life lost, then SpawnEvent {kind: "can"} after
//                  respawnSeconds of game time
//                  overrun: game over
//   SpawnEvent     kind "can": the can back in the middle, briefly shielded
//                  kind "wave": a fresh formation for wave `wave`
//
// Objects hit in a collision pass are deactivated at once, so a second
// CollisionEvent about them in the same frame is ignored. Delayed spawns
// carry the match generation, so one raised before restart() is dropped.
class Match {
public:
    enum class State { Playing, WaveCleared, Won, Lost };

    // Subscribes on construction and unsubscribes on destruction, both on the
    // thread that owns `events`, whose clock must count game microseconds.
    Match(EventManager& events, std::uint32_t seed);
    ~Match();

    Match(const Match&) = delete;
    Match& operator=(const Match&) = delete;

    // A new game: score, lives and wave 1. The formation and the can arrive
    // through SpawnEvents at the next dispatch.
    void restart();

    // Once per frame on the owning thread, then World::update(). dt is 0
    // while the game is paused.
    void update(const FrameTime& time);

    // -1 left, 0 still, 1 right. Read on the next update().
    void setMove(float axis);
    // A water drop from the can. False when the can is down, the match is
    // not being played or maxDrops are already in flight.
    bool fire();
    // The umbrella chord. False while it is open or cooling down.
    bool openUmbrella();

    World& world() { return world_; }
    const World& world() const { return world_; }
    const GameObject& can() const { return *can_; }

    State state() const { return state_; }
    int wave() const { return wave_; }
    int score() const { return score_; }
    int lives() const { return lives_; }
    int pestsAlive() const;
    int dropsInFlight() const { return countActive("drop"); }
    int seedsFalling() const { return countActive("seed"); }
    bool canUp() const { return can_->isActive(); }
    float shieldSecondsLeft() const { return shield_; }
    bool umbrellaOpen() const { return umbrella_ > 0.0F; }
    float umbrellaSecondsLeft() const { return umbrella_; }
    float umbrellaCooldownLeft() const { return cooldown_; }
    // Seeds the umbrella or the shield stopped.
    int seedsBlocked() const { return blocked_; }
    // Cosmetic objects not created because only the gameplay reserve was left.
    std::uint64_t cosmeticsSkipped() const { return skipped_; }
    // Flips every quarter second of march, for the pests' two-frame walk.
    bool marchFrame() const { return marchFrame_; }

private:
    void onCollision(const Event& event);
    void onDeath(const Event& event);
    void onSpawn(const Event& event);
    void buildWave(int wave);
    void march(float dt);
    void dropSeed(float dt);
    void sendLadybug(float dt);
    void cullStray();
    void scatter(const Transform& from, Color color, int count);
    void bloom();
    // One cosmetic bit, or false (and counted) when only the reserve is left.
    bool spawnBit(float x, float y, float velocityX, float velocityY, Color color, float seconds);
    void remove(GameObject& object);
    int countActive(const char* tag) const;
    float roll() { return unit_(rng_); }

    EventManager& events_;
    World world_{objectCapacity};
    std::vector<EventManager::SubscriptionId> subscriptions_;
    std::mt19937 rng_;
    std::uniform_real_distribution<float> unit_{0.0F, 1.0F};

    // Made once and kept: the can is deactivated while down, never destroyed.
    GameObject* can_ = nullptr;
    Transform* canTransform_ = nullptr;
    Motion* canMotion_ = nullptr;
    GameObject* fence_ = nullptr;

    State state_ = State::Playing;
    std::uint64_t generation_ = 0;
    int wave_ = 1;
    int score_ = 0;
    int lives_ = startingLives;
    float axis_ = 0.0F;
    float direction_ = 1.0F;
    float marchClock_ = 0.0F;
    bool marchFrame_ = false;
    float seedTimer_ = seedIntervalMax;
    float ladybugTimer_ = ladybugIntervalMin;
    // The way the current (or last) ladybug heads.
    bool ladybugLeftward_ = true;
    float shield_ = 0.0F;
    float umbrella_ = 0.0F;
    float cooldown_ = 0.0F;
    int blocked_ = 0;
    std::uint64_t skipped_ = 0;
};

} // namespace GardenDefense
