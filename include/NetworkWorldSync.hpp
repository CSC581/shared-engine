#pragma once

#include "Components.hpp"
#include "Multiplayer.hpp"
#include "World.hpp"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>

// Mirrors a Multiplayer::Session into a game's World, so every client keeps
// other players and shared platforms in the same object model as everything
// else in its scene.
//
// Each frame, apply() makes the World match what the session last received:
//   - a player or platform seen for the first time becomes a GameObject, built
//     by the game's factory and tagged with a NetworkIdentity
//   - one seen before has its Transform moved to the reported pose
//   - one the session no longer reports is destroyed, which is how a
//     disconnected player leaves the scene on every remaining client
// It works the same over client-server and peer-to-peer, because both deliver
// through Session.
//
// The game decides what these objects are: the factories attach a Renderable,
// a Collider, a size, anything else. Leave Motion off them: their position
// belongs to the network, and an object without Motion is never pushed by the
// local collision pass, so the local player can stand on a synced platform.
//
// Objects are tracked by ObjectId, not pointer. If the game destroys one
// itself, apply() notices once it is gone and builds it again while the
// session still reports it.
//
// Call apply() on the thread that owns the World, after session.update() and
// before world.update().
class NetworkWorldSync {
public:
    // Builds the object for a newly seen player or platform. It need not set
    // the position, add NetworkIdentity or activate the object; apply() does.
    using PlayerFactory = std::function<GameObject&(World& world, const Multiplayer::Player& player)>;
    using PlatformFactory = std::function<GameObject&(World& world, const Multiplayer::Platform& platform)>;

    // Throws std::invalid_argument if either factory is empty.
    NetworkWorldSync(World& world, PlayerFactory makePlayer, PlatformFactory makePlatform);

    NetworkWorldSync(const NetworkWorldSync&) = delete;
    NetworkWorldSync& operator=(const NetworkWorldSync&) = delete;

    void apply(const Multiplayer::Session& session);

    // The object standing for that remote player or platform, or nullptr.
    GameObject* remotePlayer(Multiplayer::PlayerId id);
    GameObject* platform(std::uint32_t id);

private:
    using Tracked = std::unordered_map<std::uint32_t, ObjectId>;

    // The tracked object for `id`, building it with `make` if it does not
    // exist (yet, or any more).
    template <class Make>
    GameObject& findOrCreate(Tracked& tracked, std::uint32_t id, NetworkIdentity::Kind kind, Make&& make);

    // Destroys every tracked object whose id is not in `seen`.
    void removeMissing(Tracked& tracked, const std::unordered_set<std::uint32_t>& seen);

    World& world_;
    PlayerFactory makePlayer_;
    PlatformFactory makePlatform_;
    Tracked players_;
    Tracked platforms_;
};
