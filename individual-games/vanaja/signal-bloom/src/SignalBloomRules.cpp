#include "SignalBloomRules.hpp"

#include "SignalBloomWorld.hpp"

#include <cmath>
#include <set>

namespace SignalBloom {

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

std::string encodeClaim(int pad)
{
    return pad >= 0 && pad < 3 ? "P" + std::to_string(pad) : "-";
}

int decodeClaim(const std::string& data)
{
    if (data.size() == 2 && data[0] == 'P' && data[1] >= '0' && data[1] <= '2') {
        return data[1] - '0';
    }
    return -1;
}

RelayStatus evaluateRelays(const std::vector<Claim>& claims)
{
    RelayStatus result;
    std::set<std::uint32_t> seen;
    for (const Claim& claim : claims) {
        if (claim.playerId == 0 || !seen.insert(claim.playerId).second) {
            continue;
        }
        const int pad = decodeClaim(claim.data);
        if (pad >= 0 && padUnderPlayer({claim.x, claim.y, playerWidth, playerHeight}) == pad) {
            result.occupied[static_cast<std::size_t>(pad)] = true;
        }
    }
    result.online = result.occupied[0] && result.occupied[1] && result.occupied[2];
    return result;
}

std::int64_t frameIntervalNs(double scale)
{
    if (!std::isfinite(scale) || (scale != 0.5 && scale != 1.0 && scale != 2.0)) {
        scale = 1.0;
    }
    return static_cast<std::int64_t>(1'000'000'000.0 / (25.0 * scale));
}

} // namespace SignalBloom
