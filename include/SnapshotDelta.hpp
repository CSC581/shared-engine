#pragma once

#include "NetworkProtocol.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Network {

enum class PlayerField : std::uint8_t {
    Name = 1U << 0U,
    X = 1U << 1U,
    Y = 1U << 2U,
    Data = 1U << 3U,
};

enum class PlatformField : std::uint8_t {
    X = 1U << 0U,
    Y = 1U << 1U,
    Width = 1U << 2U,
    Height = 1U << 3U,
};

struct PlayerPatch {
    PlayerId id = 0;
    std::uint8_t fields = 0;
    std::string name;
    float x = 0.0F;
    float y = 0.0F;
    std::string data;
};

struct PlatformPatch {
    std::uint32_t id = 0;
    std::uint8_t fields = 0;
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
};

struct SnapshotDelta {
    std::uint64_t baseTick = 0;
    std::uint64_t targetTick = 0;
    std::uint32_t targetPlayerCount = 0;
    std::uint32_t targetPlatformCount = 0;
    std::vector<PlayerState> addedPlayers;
    std::vector<PlayerId> removedPlayers;
    std::vector<PlayerPatch> changedPlayers;
    std::vector<PlatformState> addedPlatforms;
    std::vector<std::uint32_t> removedPlatforms;
    std::vector<PlatformPatch> changedPlatforms;
};

// Both functions are transport-free. makeSnapshotDelta throws
// std::invalid_argument for invalid snapshots. applySnapshotDelta validates
// the entire delta and changes result only after reconstruction succeeds.
SnapshotDelta makeSnapshotDelta(const WorldSnapshot& base, const WorldSnapshot& target);
bool applySnapshotDelta(const WorldSnapshot& base, const SnapshotDelta& delta,
                        WorldSnapshot& result, std::string& error);

} // namespace Network
