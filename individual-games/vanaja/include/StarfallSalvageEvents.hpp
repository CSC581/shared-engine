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
//   DeathEvent      breach: SpawnEvent at the spawn point, no life lost
//                   drone: a life lost, then down until revived (or, with lives
//                   left, until the beacon calls the astronaut back)
//   SpawnEvent      the only place the astronaut is placed
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

    // Once per frame on the main thread. `dt` is game seconds (0 while
    // paused). `reviveHeld` is the revive chord; `teammates` are the other
    // players' claims, whose data carries their CrewFlags.
    void update(float dt, bool reviveHeld, const std::vector<Claim>& teammates);

    // Down with no lives left, and either no teammate able to revive it or
    // restartWaitSeconds of game time spent waiting for one.
    bool canRestart(bool teammateCanHelp) const;
    // Back with full lives.
    void restartAlone();

    bool down() const { return down_; }
    // Game seconds spent down this time; 0 when not down.
    float downSeconds() const { return down_ ? downSeconds_ : 0.0F; }
    bool hasBeacon() const { return beacon_ > 0.0F; }
    float beaconSecondsLeft() const { return beacon_ > 0.0F ? beacon_ : 0.0F; }
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
    void show(Banner::Kind kind, std::uint32_t subject, std::uint32_t actor);

    EventManager& events_;
    GameObject& player_;
    Astronaut& astronaut_;
    const Station& station_;
    Hooks hooks_;
    std::vector<EventManager::SubscriptionId> subscriptions_;

    bool down_ = false;
    // Seconds until a downed astronaut calls itself back; <= 0 means never.
    float beacon_ = 0.0F;
    float downSeconds_ = 0.0F;
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
