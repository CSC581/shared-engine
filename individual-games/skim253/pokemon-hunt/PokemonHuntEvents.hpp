#pragma once

#include "Event.hpp"
#include "GameObject.hpp"
#include "PokemonHuntWorld.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Game-specific event types and local event helpers.
namespace PokemonHunt {
namespace GameEvents {

// Hit {kind, points, skill, x, y}; Mewtwo validates the hit and derives damage from kind.
constexpr const char* Hit = "HitEvent";
constexpr const char* HitStomp = "stomp";
constexpr const char* HitShot = "shot";

// Psystrike {stage, x, at, seconds?}: charge or strike at boss time `at` (microseconds).
// x is the origin of both shockwaves; seconds is the charge duration.
constexpr const char* Psystrike = "PsystrikeEvent";

// Ping {sentAt}; Pong {player, sentAt, bossTime}.
// sentAt is client real time in nanoseconds; bossTime is boss time in microseconds.
constexpr const char* ClockPing = "ClockPingEvent";
constexpr const char* ClockPong = "ClockPongEvent";

// Private delayed event {attack}; stale charge reminders are ignored.
constexpr const char* PsystrikeDue = "PsystrikeDueEvent";

} // namespace GameEvents

// Shared by clients and the boss bot; Spawn stays local.
inline std::vector<std::string> sharedEventTypes()
{
    return {GameEvents::Hit, GameEvents::Psystrike, Events::Death, GameEvents::ClockPing, GameEvents::ClockPong};
}

// Mewtwo's damage for a hit kind; 0 for a kind it does not know.
inline int damageFor(const std::string& kind)
{
    if (kind == GameEvents::HitStomp) {
        return stompDamage;
    }
    return kind == GameEvents::HitShot ? shotDamage : 0;
}

// DeathEvent {object, cause}. High: a death must never be dropped from a full queue.
inline Event deathEvent(ObjectId object, const char* cause)
{
    Event death{Events::Death};
    death.priority = EventPriority::High;
    death.set("object", static_cast<std::int64_t>(object)).set("cause", cause);
    return death;
}

// SpawnEvent {object, x, y}: put an existing object back at (x, y).
inline Event respawnEvent(ObjectId object, float x, float y)
{
    Event spawn{Events::Spawn};
    spawn.priority = EventPriority::High; // A death must always get its respawn.
    spawn.set("object", static_cast<std::int64_t>(object))
        .set("x", static_cast<double>(x))
        .set("y", static_cast<double>(y));
    return spawn;
}

// A Death or Spawn raised on this machine about `object`. Remote ones carry
// no object, since ids are local.
inline bool isLocalAbout(const Event& event, ObjectId object)
{
    return event.origin == 0 && event.getOr<std::int64_t>("object", 0) == static_cast<std::int64_t>(object);
}

// The other side of a CollisionEvent that involves `self`.
struct Contact {
    ObjectId other = 0;
    std::string otherTag;
    std::string phase;
};

inline bool contactOf(const Event& collision, ObjectId self, Contact& contact)
{
    const auto a = static_cast<ObjectId>(collision.get<std::int64_t>("a"));
    const auto b = static_cast<ObjectId>(collision.get<std::int64_t>("b"));
    if (a != self && b != self) {
        return false;
    }
    contact.other = a == self ? b : a;
    contact.otherTag = collision.get<std::string>(a == self ? "tagB" : "tagA");
    contact.phase = collision.get<std::string>("phase");
    return true;
}

} // namespace PokemonHunt
