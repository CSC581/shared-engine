#include "NetworkWorldSync.hpp"

#include <stdexcept>
#include <utility>

NetworkWorldSync::NetworkWorldSync(World& world, PlayerFactory makePlayer, PlatformFactory makePlatform)
    : world_(world),
      makePlayer_(std::move(makePlayer)),
      makePlatform_(std::move(makePlatform))
{
    if (!makePlayer_ || !makePlatform_) {
        throw std::invalid_argument("NetworkWorldSync needs a player and a platform factory");
    }
}

void NetworkWorldSync::apply(const Multiplayer::Session& session)
{
    std::unordered_set<std::uint32_t> seen;

    for (const Multiplayer::Player& player : session.remotePlayers()) {
        GameObject& object = findOrCreate(players_, player.id, NetworkIdentity::Kind::Player,
                                          [&]() -> GameObject& { return makePlayer_(world_, player); });
        Transform& transform = *object.get<Transform>();
        transform.x = player.x;
        transform.y = player.y;
        seen.insert(player.id);
    }
    removeMissing(players_, seen);

    seen.clear();
    for (const Multiplayer::Platform& platform : session.platforms()) {
        GameObject& object = findOrCreate(platforms_, platform.id, NetworkIdentity::Kind::Platform,
                                          [&]() -> GameObject& { return makePlatform_(world_, platform); });
        Transform& transform = *object.get<Transform>();
        transform.x = platform.x;
        transform.y = platform.y;
        transform.width = platform.width;
        transform.height = platform.height;
        seen.insert(platform.id);
    }
    removeMissing(platforms_, seen);
}

GameObject* NetworkWorldSync::remotePlayer(Multiplayer::PlayerId id)
{
    const auto tracked = players_.find(id);
    return tracked == players_.end() ? nullptr : world_.find(tracked->second);
}

GameObject* NetworkWorldSync::platform(std::uint32_t id)
{
    const auto tracked = platforms_.find(id);
    return tracked == platforms_.end() ? nullptr : world_.find(tracked->second);
}

template <class Make>
GameObject& NetworkWorldSync::findOrCreate(Tracked& tracked, std::uint32_t id, NetworkIdentity::Kind kind, Make&& make)
{
    const auto existing = tracked.find(id);
    if (existing != tracked.end()) {
        if (GameObject* object = world_.find(existing->second)) {
            return *object;
        }
    }

    GameObject& object = make();
    object.require<Transform>();
    if (!object.has<NetworkIdentity>()) {
        object.add<NetworkIdentity>(kind, id);
    }
    object.setActive(true);
    tracked[id] = object.id();
    return object;
}

void NetworkWorldSync::removeMissing(Tracked& tracked, const std::unordered_set<std::uint32_t>& seen)
{
    for (auto entry = tracked.begin(); entry != tracked.end();) {
        if (seen.count(entry->first) == 0) {
            world_.destroy(entry->second);
            entry = tracked.erase(entry);
        } else {
            ++entry;
        }
    }
}
