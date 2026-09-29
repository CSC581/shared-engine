#include "StarfallSalvageRules.hpp"

#include "StarfallSalvageWorld.hpp"

#include <cmath>
#include <set>

namespace StarfallSalvage {

int padUnderPlayer(const Rect& player)
{
    const float footX = player.x + player.width * 0.5F;
    const float footY = player.y + player.height;
    for (std::size_t i = 0; i < relayPads.size(); ++i) {
        const Rect& pad = relayPads[i];
        if (footX >= pad.x && footX <= pad.x + pad.width &&
            std::fabs(footY - (pad.y + pad.height)) <= 4.0F) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int gatePlateUnderPlayer(const Rect& player)
{
    const float footX = player.x + player.width * 0.5F;
    const float footY = player.y + player.height;
    for (std::size_t i = 0; i < gatePlates.size(); ++i) {
        const Rect& plate = gatePlates[i];
        if (footX >= plate.x && footX <= plate.x + plate.width &&
            std::fabs(footY - (plate.y + plate.height)) <= 4.0F) {
            return static_cast<int>(relayPads.size() + i);
        }
    }
    return -1;
}

std::string encodeState(std::uint8_t cargoMask, int control,
                        std::uint8_t personalCargoMask)
{
    constexpr char hex[] = "0123456789ABCDEF";
    return std::string{'C', hex[cargoMask & allCargoMask], 'P',
                       control >= 0 && control < static_cast<int>(relayPads.size() + gatePlates.size())
                           ? static_cast<char>('0' + control) : '-',
                       'I', hex[personalCargoMask & cargoMask & allCargoMask]};
}

bool decodeState(const std::string& data, std::uint8_t& cargoMask, int& control,
                 std::uint8_t& personalCargoMask)
{
    if (data.size() != 6 || data[0] != 'C' || data[2] != 'P' || data[4] != 'I') {
        return false;
    }
    constexpr char hex[] = "0123456789ABCDEF";
    const std::size_t mask = std::string(hex).find(data[1]);
    const std::size_t personalMask = std::string(hex).find(data[5]);
    const int controlCount = static_cast<int>(relayPads.size() + gatePlates.size());
    if (mask == std::string::npos || mask > allCargoMask ||
        personalMask == std::string::npos || personalMask > allCargoMask ||
        (personalMask & ~mask) != 0 ||
        (data[3] != '-' && (data[3] < '0' ||
                            data[3] >= '0' + controlCount))) {
        return false;
    }
    cargoMask = static_cast<std::uint8_t>(mask);
    control = data[3] == '-' ? -1 : data[3] - '0';
    personalCargoMask = static_cast<std::uint8_t>(personalMask);
    return true;
}

MissionStatus evaluateMission(const std::vector<Claim>& claims)
{
    MissionStatus result;
    std::set<std::uint32_t> seen;
    for (const Claim& claim : claims) {
        if (claim.playerId == 0 || !seen.insert(claim.playerId).second) {
            continue;
        }
        std::uint8_t mask = 0;
        std::uint8_t personalMask = 0;
        int control = -1;
        if (!decodeState(claim.data, mask, control, personalMask)) {
            continue;
        }
        result.cargoMask |= mask;
        const Rect player{claim.x, claim.y, playerWidth, playerHeight};
        if (control >= 0 && control < static_cast<int>(relayPads.size()) &&
            padUnderPlayer(player) == control) {
            result.relays.occupied[static_cast<std::size_t>(control)] = true;
        } else if (control >= static_cast<int>(relayPads.size()) &&
                   gatePlateUnderPlayer(player) == control) {
            result.gateOpen = true;
        }
    }
    result.relays.online = result.relays.occupied[0] && result.relays.occupied[1] &&
                           result.relays.occupied[2];
    result.complete = result.cargoMask == allCargoMask && result.relays.online;
    return result;
}

CargoWinner chooseCargoWinner(const std::vector<Claim>& claims)
{
    std::set<std::uint32_t> seen;
    CargoWinner winner;
    for (const Claim& claim : claims) {
        if (claim.playerId == 0 || !seen.insert(claim.playerId).second) continue;
        std::uint8_t mask = 0;
        std::uint8_t personalMask = 0;
        int control = -1;
        if (!decodeState(claim.data, mask, control, personalMask)) continue;
        int points = 0;
        for (std::size_t i = 0; i < cargo.size(); ++i) {
            points += (personalMask >> i) & 1U;
        }
        if (winner.playerId == 0 || points > winner.crates ||
            (points == winner.crates && claim.playerId < winner.playerId)) {
            winner = {claim.playerId, points};
        }
    }
    return winner;
}

std::int64_t frameIntervalNs(double scale)
{
    if (!std::isfinite(scale) || (scale != 0.5 && scale != 1.0 && scale != 2.0)) {
        scale = 1.0;
    }
    return static_cast<std::int64_t>(1'000'000'000.0 / (25.0 * scale));
}

} // namespace StarfallSalvage
