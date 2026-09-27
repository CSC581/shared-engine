#include "SignalBloomRules.hpp"
#include "SignalBloomWorld.hpp"

#include <iostream>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { \
        std::cerr << "FAILED: " #condition " at line " << __LINE__ << '\n'; \
        return 1; \
    } \
} while (false)

namespace {

SignalBloom::Claim onPad(std::uint32_t id, int pad)
{
    const Rect area = SignalBloom::relayPads[static_cast<std::size_t>(pad)];
    return {id, area.x + 10.0F,
            area.y + area.height - SignalBloom::playerHeight,
            SignalBloom::encodeClaim(pad)};
}

} // namespace

int main()
{
    using SignalBloom::evaluateRelays;

    EXPECT(SignalBloom::decodeClaim("P0") == 0);
    EXPECT(SignalBloom::decodeClaim("P2") == 2);
    EXPECT(SignalBloom::decodeClaim("P3") == -1);
    EXPECT(SignalBloom::decodeClaim("P0extra") == -1);
    EXPECT(SignalBloom::decodeClaim("-") == -1);
    EXPECT(SignalBloom::encodeClaim(-1) == "-");

    std::vector<SignalBloom::Claim> players{onPad(1, 0), onPad(2, 1), onPad(3, 2)};
    auto result = evaluateRelays(players);
    EXPECT(result.online);
    EXPECT(result.occupied[0] && result.occupied[1] && result.occupied[2]);

    players.pop_back();
    EXPECT(!evaluateRelays(players).online);
    players.push_back(onPad(2, 2)); // duplicate ID cannot operate two relays
    EXPECT(!evaluateRelays(players).online);
    players.back() = onPad(3, 2);
    players.back().x = 40.0F; // a stale or forged pad claim is not enough
    EXPECT(!evaluateRelays(players).online);
    players.back() = onPad(3, 2);
    players.back().data = "garbage";
    EXPECT(!evaluateRelays(players).online);
    players.back() = onPad(3, 2);
    EXPECT(evaluateRelays(players).online);

    players.erase(players.begin() + 1); // leave/timeout removes the claim
    EXPECT(!evaluateRelays(players).online);
    EXPECT(SignalBloom::padUnderPlayer({40.0F, 360.0F, 30.0F, 40.0F}) == -1);
    EXPECT(SignalBloom::padUnderPlayer({onPad(1, 0).x, onPad(1, 0).y,
                                       SignalBloom::playerWidth,
                                       SignalBloom::playerHeight}) == 0);

    EXPECT(SignalBloom::frameIntervalNs(0.5) == 80'000'000);
    EXPECT(SignalBloom::frameIntervalNs(1.0) == 40'000'000);
    EXPECT(SignalBloom::frameIntervalNs(2.0) == 20'000'000);
    EXPECT(SignalBloom::frameIntervalNs(0.0) == 40'000'000);

    const auto config = SignalBloom::serverConfig();
    EXPECT(config.platforms.size() == 2);
    EXPECT(config.spawnPoints.size() == 8);
    EXPECT(config.platforms[0].endX > config.platforms[0].startX);
    EXPECT(config.platforms[1].endX > config.platforms[1].startX);

    std::cout << "Signal Bloom rules passed\n";
}
