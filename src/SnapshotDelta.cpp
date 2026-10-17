#include "SnapshotDelta.hpp"

#include "WireFormat.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace Network {
namespace {

constexpr std::uint8_t playerMask = 0x0FU;
constexpr std::uint8_t platformMask = 0x0FU;

constexpr std::uint8_t bit(PlayerField field)
{
    return static_cast<std::uint8_t>(field);
}

constexpr std::uint8_t bit(PlatformField field)
{
    return static_cast<std::uint8_t>(field);
}

bool sameFloat(float left, float right)
{
    std::uint32_t leftBits = 0;
    std::uint32_t rightBits = 0;
    static_assert(sizeof(leftBits) == sizeof(left), "32-bit floats are required");
    std::memcpy(&leftBits, &left, sizeof(left));
    std::memcpy(&rightBits, &right, sizeof(right));
    return leftBits == rightBits;
}

bool validPlayer(const PlayerState& player)
{
    return player.id != 0 && Net::isValidName(player.name) &&
           Net::isValidPlayerData(player.data) && std::isfinite(player.x) &&
           std::isfinite(player.y);
}

bool validPlatform(const PlatformState& platform)
{
    return platform.id != 0 && std::isfinite(platform.x) &&
           std::isfinite(platform.y) && std::isfinite(platform.width) &&
           std::isfinite(platform.height) && platform.width > 0.0F &&
           platform.height > 0.0F;
}

template <typename State, typename Id, typename Valid>
std::map<Id, State> indexed(const std::vector<State>& states, Valid valid,
                            const char* description)
{
    std::map<Id, State> result;
    for (const State& state : states) {
        if (!valid(state) || !result.emplace(state.id, state).second) {
            throw std::invalid_argument(std::string("invalid or duplicate ") + description);
        }
    }
    return result;
}

using Players = std::map<PlayerId, PlayerState>;
using Platforms = std::map<std::uint32_t, PlatformState>;

Players players(const WorldSnapshot& snapshot)
{
    return indexed<PlayerState, PlayerId>(snapshot.players, validPlayer, "player in snapshot");
}

Platforms platforms(const WorldSnapshot& snapshot)
{
    return indexed<PlatformState, std::uint32_t>(snapshot.platforms, validPlatform,
                                                  "platform in snapshot");
}

template <typename Id>
bool claim(Id id, std::set<Id>& seen, const char* description, std::string& error)
{
    if (id == 0 || !seen.insert(id).second) {
        error = std::string("invalid or repeated ") + description + " ID";
        return false;
    }
    return true;
}

template <typename State, typename Map>
std::vector<State> values(const Map& map)
{
    std::vector<State> result;
    result.reserve(map.size());
    for (const auto& entry : map) {
        result.push_back(entry.second);
    }
    return result;
}

} // namespace

SnapshotDelta makeSnapshotDelta(const WorldSnapshot& base, const WorldSnapshot& target)
{
    if (target.serverTick <= base.serverTick) {
        throw std::invalid_argument("snapshot delta target tick must exceed its base tick");
    }
    if (target.players.size() > std::numeric_limits<std::uint32_t>::max() ||
        target.platforms.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("snapshot delta target has too many objects");
    }

    const Players oldPlayers = players(base);
    const Players newPlayers = players(target);
    const Platforms oldPlatforms = platforms(base);
    const Platforms newPlatforms = platforms(target);

    SnapshotDelta delta;
    delta.baseTick = base.serverTick;
    delta.targetTick = target.serverTick;
    delta.targetPlayerCount = static_cast<std::uint32_t>(newPlayers.size());
    delta.targetPlatformCount = static_cast<std::uint32_t>(newPlatforms.size());

    for (const auto& entry : newPlayers) {
        const auto previous = oldPlayers.find(entry.first);
        if (previous == oldPlayers.end()) {
            delta.addedPlayers.push_back(entry.second);
            continue;
        }
        const PlayerState& before = previous->second;
        const PlayerState& after = entry.second;
        PlayerPatch patch;
        patch.id = entry.first;
        if (before.name != after.name) {
            patch.fields |= bit(PlayerField::Name);
            patch.name = after.name;
        }
        if (!sameFloat(before.x, after.x)) {
            patch.fields |= bit(PlayerField::X);
            patch.x = after.x;
        }
        if (!sameFloat(before.y, after.y)) {
            patch.fields |= bit(PlayerField::Y);
            patch.y = after.y;
        }
        if (before.data != after.data) {
            patch.fields |= bit(PlayerField::Data);
            patch.data = after.data;
        }
        if (patch.fields != 0) {
            delta.changedPlayers.push_back(std::move(patch));
        }
    }
    for (const auto& entry : oldPlayers) {
        if (newPlayers.count(entry.first) == 0) {
            delta.removedPlayers.push_back(entry.first);
        }
    }

    for (const auto& entry : newPlatforms) {
        const auto previous = oldPlatforms.find(entry.first);
        if (previous == oldPlatforms.end()) {
            delta.addedPlatforms.push_back(entry.second);
            continue;
        }
        const PlatformState& before = previous->second;
        const PlatformState& after = entry.second;
        PlatformPatch patch;
        patch.id = entry.first;
        const auto property = [&](float oldValue, float newValue, PlatformField field,
                                  float& destination) {
            if (!sameFloat(oldValue, newValue)) {
                patch.fields |= bit(field);
                destination = newValue;
            }
        };
        property(before.x, after.x, PlatformField::X, patch.x);
        property(before.y, after.y, PlatformField::Y, patch.y);
        property(before.width, after.width, PlatformField::Width, patch.width);
        property(before.height, after.height, PlatformField::Height, patch.height);
        if (patch.fields != 0) {
            delta.changedPlatforms.push_back(patch);
        }
    }
    for (const auto& entry : oldPlatforms) {
        if (newPlatforms.count(entry.first) == 0) {
            delta.removedPlatforms.push_back(entry.first);
        }
    }
    return delta;
}

bool applySnapshotDelta(const WorldSnapshot& base, const SnapshotDelta& delta,
                        WorldSnapshot& result, std::string& error)
{
    error.clear();
    if (base.serverTick != delta.baseTick) {
        error = "snapshot delta baseline tick mismatch";
        return false;
    }
    if (delta.targetTick <= delta.baseTick) {
        error = "snapshot delta target tick must exceed its base tick";
        return false;
    }

    Players nextPlayers;
    Platforms nextPlatforms;
    try {
        nextPlayers = players(base);
        nextPlatforms = platforms(base);
    } catch (const std::invalid_argument& exception) {
        error = exception.what();
        return false;
    }

    std::set<PlayerId> seenPlayers;
    for (const PlayerState& player : delta.addedPlayers) {
        if (!claim(player.id, seenPlayers, "player delta", error) || !validPlayer(player) ||
            nextPlayers.count(player.id) != 0) {
            if (error.empty()) error = "invalid added player";
            return false;
        }
        nextPlayers.emplace(player.id, player);
    }
    for (PlayerId id : delta.removedPlayers) {
        if (!claim(id, seenPlayers, "player delta", error) || nextPlayers.erase(id) != 1) {
            if (error.empty()) error = "removed player does not exist";
            return false;
        }
    }
    for (const PlayerPatch& patch : delta.changedPlayers) {
        if (!claim(patch.id, seenPlayers, "player delta", error) || patch.fields == 0 ||
            (patch.fields & ~playerMask) != 0) {
            if (error.empty()) error = "invalid player patch mask";
            return false;
        }
        const auto found = nextPlayers.find(patch.id);
        if (found == nextPlayers.end()) {
            error = "changed player does not exist";
            return false;
        }
        PlayerState& player = found->second;
        if ((patch.fields & bit(PlayerField::Name)) != 0) player.name = patch.name;
        if ((patch.fields & bit(PlayerField::X)) != 0) player.x = patch.x;
        if ((patch.fields & bit(PlayerField::Y)) != 0) player.y = patch.y;
        if ((patch.fields & bit(PlayerField::Data)) != 0) player.data = patch.data;
        if (!validPlayer(player)) {
            error = "invalid changed player";
            return false;
        }
    }

    std::set<std::uint32_t> seenPlatforms;
    for (const PlatformState& platform : delta.addedPlatforms) {
        if (!claim(platform.id, seenPlatforms, "platform delta", error) ||
            !validPlatform(platform) || nextPlatforms.count(platform.id) != 0) {
            if (error.empty()) error = "invalid added platform";
            return false;
        }
        nextPlatforms.emplace(platform.id, platform);
    }
    for (std::uint32_t id : delta.removedPlatforms) {
        if (!claim(id, seenPlatforms, "platform delta", error) ||
            nextPlatforms.erase(id) != 1) {
            if (error.empty()) error = "removed platform does not exist";
            return false;
        }
    }
    for (const PlatformPatch& patch : delta.changedPlatforms) {
        if (!claim(patch.id, seenPlatforms, "platform delta", error) ||
            patch.fields == 0 || (patch.fields & ~platformMask) != 0) {
            if (error.empty()) error = "invalid platform patch mask";
            return false;
        }
        const auto found = nextPlatforms.find(patch.id);
        if (found == nextPlatforms.end()) {
            error = "changed platform does not exist";
            return false;
        }
        PlatformState& platform = found->second;
        if ((patch.fields & bit(PlatformField::X)) != 0) platform.x = patch.x;
        if ((patch.fields & bit(PlatformField::Y)) != 0) platform.y = patch.y;
        if ((patch.fields & bit(PlatformField::Width)) != 0) platform.width = patch.width;
        if ((patch.fields & bit(PlatformField::Height)) != 0) platform.height = patch.height;
        if (!validPlatform(platform)) {
            error = "invalid changed platform";
            return false;
        }
    }

    if (nextPlayers.size() != delta.targetPlayerCount ||
        nextPlatforms.size() != delta.targetPlatformCount) {
        error = "snapshot delta target object count mismatch";
        return false;
    }

    WorldSnapshot candidate;
    candidate.serverTick = delta.targetTick;
    candidate.players = values<PlayerState>(nextPlayers);
    candidate.platforms = values<PlatformState>(nextPlatforms);
    result = std::move(candidate);
    return true;
}

} // namespace Network
