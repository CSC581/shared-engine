#pragma once

#include "Collision.hpp"
#include "EventManager.hpp"
#include "StarfallSalvageLevel.hpp"
#include "StarfallSalvageRules.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Starfall's gameplay as event handlers on the engine's EventManager.
// SDL-free, so tests drive it with a ManualClock and a World.
//
//   CollisionEvent  player + drone       -> DeathEvent {cause: "drone"}, on
//                                           "enter" or "stay" (not while shielded)
//                   player + deathzone   -> DeathEvent {cause: "breach"}
//                   player + cargo       -> the crate is collected
//                   player + gravitypanel-> gravity flips
//   DeathEvent      breach: SpawnEvent at the spawn point at once
//                   drone: a life lost, then down until revived or until the
//                   beacon calls the astronaut back (later after the last life)
//   SpawnEvent      the only place the astronaut is placed; restores every
//                   life if the last one was lost
//   ReviveEvent     a teammate rescued the downed player `target`; honoured
//                   only if the rescuer is within 2 x reviveRange of this player
//   StationAlarmEvent (from this player or the current alarm director, the
//                   connected player with the lowest id) -> AlarmBeginEvent and
//                   AlarmEndEvent, raised with delays on game time
//
// Only events this machine raised (origin 0) change the local astronaut.
// Another player's DeathEvent or ReviveEvent arrives through the
// NetworkEventBridge and only produces a banner, except a ReviveEvent naming
// this player, which brings it back at the rescuer's side.
namespace StarfallSalvage {

// A short message about someone, kept until it expires.
struct Banner {
    enum class Kind { None, Down, Breach, Revived };
    Kind kind = Kind::None;
    // Player ids; 0 means this player.
    std::uint32_t subject = 0;
    std::uint32_t actor = 0;
    float seconds = 0.0F;
};

class StationEvents {
public:
    struct Hooks {
        // A crate was touched. Returns true if it was newly collected.
        std::function<bool(std::size_t crate)> collectCargo;
        // Where another player is; false when unknown.
        std::function<bool(std::uint32_t id, Rect& bounds)> findPlayer;
        // This player's id, 0 before the session is ready.
        std::function<std::uint32_t()> localPlayerId;
        // The player whose StationAlarmEvents are trusted: the connected
        // player with the lowest id. Unset or 0 trusts only local alarms.
        std::function<std::uint32_t()> alarmDirectorId;
    };

    // Subscribes on construction and unsubscribes on destruction, both on the
    // thread that owns `events`. `player` must carry an Astronaut.
    StationEvents(EventManager& events, GameObject& player, const Station& station, Hooks hooks);
    ~StationEvents();

    StationEvents(const StationEvents&) = delete;
    StationEvents& operator=(const StationEvents&) = delete;

    // The first SpawnEvent, once the game has placed the astronaut at its
    // spawn point: it gets the spawn shield and any gravity failure under way.
    void spawnAtStart();

    // Once per frame on the main thread. `dt` is game seconds (0 while
    // paused). `reviveHeld` is the revive chord; `teammates` are the other
    // players' claims, whose data carries their CrewFlags.
    void update(float dt, bool reviveHeld, const std::vector<Claim>& teammates);

    bool down() const { return down_; }
    // Game seconds until the beacon calls a downed astronaut back; 0 when not down.
    float beaconSecondsLeft() const { return beacon_; }
    std::uint32_t reviveTargetId() const { return reviveTarget_; }
    // 0 to 1.
    float reviveProgress() const { return reviveProgress_ / reviveSeconds; }

    bool gravityFailure() const { return gravityAlarms_ > 0; }
    bool blackout() const { return blackoutAlarms_ > 0; }
    // The alarm about to begin ("gravity" or "blackout"), or empty.
    const std::string& alarmWarning() const { return warningKind_; }
    float warningSecondsLeft() const { return warningSeconds_; }

    const Banner& banner() const { return banner_; }
    CrewFlags flags() const;

private:
    void onCollision(const Event& event);
    void onDeath(const Event& event);
    void onSpawn(const Event& event);
    void onRevive(const Event& event);
    void onAlarm(const Event& event);
    void onAlarmEdge(const Event& event, int change);
    void applyGravity();
    std::uint32_t localId() const { return hooks_.localPlayerId ? hooks_.localPlayerId() : 0; }
    void show(Banner::Kind kind, std::uint32_t subject, std::uint32_t actor);

    EventManager& events_;
    GameObject& player_;
    Astronaut& astronaut_;
    const Station& station_;
    Hooks hooks_;
    std::vector<EventManager::SubscriptionId> subscriptions_;

    bool down_ = false;
    // A DeathEvent this player raised and has not handled yet. Every hazard
    // touched in one collision pass raises its CollisionEvent before any
    // death is handled, so only the first may raise a death; set until the
    // next update().
    bool deathPending_ = false;
    // Seconds until a downed astronaut calls itself back.
    float beacon_ = 0.0F;
    bool panelFlipped_ = false;
    float panelCooldown_ = 0.0F;
    std::uint32_t reviveTarget_ = 0;
    float reviveProgress_ = 0.0F;
    int gravityAlarms_ = 0;
    int blackoutAlarms_ = 0;
    std::string warningKind_;
    float warningSeconds_ = 0.0F;
    Banner banner_{};
};

} // namespace StarfallSalvage
