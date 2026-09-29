#pragma once

#include "Entity.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace StarfallSalvage {

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

struct MissionStatus {
    RelayStatus relays;
    std::uint8_t cargoMask = 0;
    bool gateOpen = false;
    bool complete = false;
};

struct CargoWinner {
    std::uint32_t playerId = 0;
    int crates = 0;
};

// One player can claim only the pad beneath their feet, and only while holding E.
int padUnderPlayer(const Rect& player);
int gatePlateUnderPlayer(const Rect& player);
std::string encodeState(std::uint8_t cargoMask, int control, std::uint8_t personalCargoMask);
bool decodeState(const std::string& data, std::uint8_t& cargoMask, int& control,
                 std::uint8_t& personalCargoMask);
MissionStatus evaluateMission(const std::vector<Claim>& claims);
CargoWinner chooseCargoWinner(const std::vector<Claim>& claims);

// A 25 Hz baseline leaves headroom beneath the engine's 50 ms game-delta cap
// at every supported scale: 12.5, 25, or 50 actual loop iterations per second.
std::int64_t frameIntervalNs(double scale);

} // namespace StarfallSalvage
