#pragma once

#include "Entity.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace SignalBloom {

struct Claim {
    std::uint32_t playerId = 0;
    float x = 0.0F;
    float y = 0.0F;
    std::string data;
};

struct RelayStatus {
    std::array<bool, 3> occupied{{false, false, false}};
    bool online = false;
};

// One player can claim only the pad beneath their feet, and only while holding E.
int padUnderPlayer(const Rect& player);
std::string encodeClaim(int pad);
int decodeClaim(const std::string& data);
RelayStatus evaluateRelays(const std::vector<Claim>& claims);

// A 25 Hz baseline leaves headroom beneath the engine's 50 ms game-delta cap
// at every supported scale: 12.5, 25, or 50 actual loop iterations per second.
std::int64_t frameIntervalNs(double scale);

} // namespace SignalBloom
