#include "StarfallSalvageRules.hpp"
#include "StarfallSalvageWorld.hpp"

#include <algorithm>
#include <iostream>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { \
        std::cerr << "FAILED: " #condition " at line " << __LINE__ << '\n'; \
        return 1; \
    } \
} while (false)

namespace {

StarfallSalvage::Claim onPad(std::uint32_t id, int pad, std::uint8_t cargoMask)
{
    const Rect area = StarfallSalvage::relayPads[static_cast<std::size_t>(pad)];
    return {id, area.x + 10.0F,
            area.y + area.height - StarfallSalvage::playerHeight,
            StarfallSalvage::encodeState(cargoMask, pad, cargoMask)};
}

StarfallSalvage::Claim onGate(std::uint32_t id, int plate)
{
    const Rect area = StarfallSalvage::gatePlates[static_cast<std::size_t>(plate)];
    return {id, area.x + 10.0F,
            area.y + area.height - StarfallSalvage::playerHeight,
            StarfallSalvage::encodeState(0, static_cast<int>(StarfallSalvage::relayPads.size()) + plate,
                                     0)};
}

bool supportedByDeck(const Rect& object)
{
    return std::any_of(StarfallSalvage::decks.begin(), StarfallSalvage::decks.end(),
                       [&object](const Rect& deck) {
                           return object.x >= deck.x &&
                                  object.x + object.width <= deck.x + deck.width &&
                                  object.y + object.height == deck.y;
                       });
}

} // namespace

int main()
{
    using StarfallSalvage::evaluateMission;

    std::uint8_t mask = 0;
    std::uint8_t personalMask = 0;
    int pad = -1;
    EXPECT(StarfallSalvage::encodeState(0xA, 2, 0x2) == "CAP2I2");
    EXPECT(StarfallSalvage::encodeState(0xF, -1, 0) == "CFP-I0");
    EXPECT(StarfallSalvage::decodeState("C3P0I1", mask, pad, personalMask) &&
           mask == 3 && pad == 0 && personalMask == 1);
    EXPECT(StarfallSalvage::decodeState("CFP-I0", mask, pad, personalMask) &&
           mask == 15 && pad == -1 && personalMask == 0);
    EXPECT(StarfallSalvage::decodeState("C0P3I0", mask, pad, personalMask) && pad == 3);
    EXPECT(StarfallSalvage::decodeState("C0P4I0", mask, pad, personalMask) && pad == 4);
    EXPECT(!StarfallSalvage::decodeState("P0", mask, pad, personalMask));
    EXPECT(!StarfallSalvage::decodeState("C3P0", mask, pad, personalMask));
    EXPECT(!StarfallSalvage::decodeState("C0P5I0", mask, pad, personalMask));
    EXPECT(!StarfallSalvage::decodeState("C0P-I1", mask, pad, personalMask));
    EXPECT(!StarfallSalvage::decodeState("CgP0I0", mask, pad, personalMask));
    EXPECT(!StarfallSalvage::decodeState("CFP0extra", mask, pad, personalMask));

    std::vector<StarfallSalvage::Claim> players{
        onPad(1, 0, 0x1), onPad(2, 1, 0x2), onPad(3, 2, 0xC)
    };
    auto result = evaluateMission(players);
    EXPECT(result.complete);
    EXPECT(result.cargoMask == StarfallSalvage::allCargoMask);
    EXPECT(result.relays.occupied[0] && result.relays.occupied[1] && result.relays.occupied[2]);
    EXPECT(StarfallSalvage::chooseCargoWinner(players).playerId == 3);
    EXPECT(StarfallSalvage::chooseCargoWinner(players).crates == 2);

    const std::vector<StarfallSalvage::Claim> simultaneousPickup{
        {4, 0.0F, 0.0F, StarfallSalvage::encodeState(0x1, -1, 0x1)},
        {2, 0.0F, 0.0F, StarfallSalvage::encodeState(0x3, -1, 0x3)},
        {3, 0.0F, 0.0F, StarfallSalvage::encodeState(0x4, -1, 0x4)},
    };
    EXPECT(StarfallSalvage::chooseCargoWinner(simultaneousPickup).playerId == 2);
    EXPECT(StarfallSalvage::chooseCargoWinner(simultaneousPickup).crates == 2);
    EXPECT(StarfallSalvage::chooseCargoWinner({onPad(9, 0, 1), onPad(2, 1, 2)}).playerId == 2);
    EXPECT(StarfallSalvage::chooseCargoWinner({{1, 0.0F, 0.0F, "bad"}, onPad(2, 1, 2)}).playerId == 2);
    EXPECT(StarfallSalvage::chooseCargoWinner({}).playerId == 0);

    players.pop_back();
    EXPECT(!evaluateMission(players).complete);
    players.push_back(onPad(2, 2, 0xC)); // duplicate ID cannot operate two relays
    EXPECT(!evaluateMission(players).complete);
    players.back() = onPad(3, 2, 0xC);
    players.back().x = 40.0F; // a stale or forged pad claim is not enough
    EXPECT(!evaluateMission(players).complete);
    players.back() = onPad(3, 2, 0xC);
    players.back().data = "garbage";
    EXPECT(!evaluateMission(players).complete);
    players.back() = onPad(3, 2, 0x4);
    EXPECT(evaluateMission(players).relays.online);
    EXPECT(!evaluateMission(players).complete); // all relays are held, but cargo is missing
    players.back() = onPad(3, 2, 0xC);
    EXPECT(evaluateMission(players).complete);

    const auto lateJoinView = evaluateMission({onPad(1, 0, StarfallSalvage::allCargoMask)});
    EXPECT(lateJoinView.cargoMask == StarfallSalvage::allCargoMask);
    EXPECT(!lateJoinView.complete); // progress survives in a peer, not a lone relay

    players.erase(players.begin() + 1); // leave/timeout removes the claim
    EXPECT(!evaluateMission(players).complete);
    EXPECT(StarfallSalvage::padUnderPlayer({40.0F, 360.0F, 30.0F, 40.0F}) == -1);
    EXPECT(StarfallSalvage::padUnderPlayer({onPad(1, 0, 0).x, onPad(1, 0, 0).y,
                                       StarfallSalvage::playerWidth,
                                       StarfallSalvage::playerHeight}) == 0);

    auto gateClaim = onGate(1, 0);
    EXPECT(evaluateMission({gateClaim}).gateOpen);
    EXPECT(!evaluateMission({}).gateOpen);
    gateClaim.x = 40.0F;
    EXPECT(!evaluateMission({gateClaim}).gateOpen);
    gateClaim = onGate(2, 1);
    EXPECT(evaluateMission({gateClaim}).gateOpen);
    gateClaim.data = "C0P5I0";
    EXPECT(!evaluateMission({gateClaim}).gateOpen);
    EXPECT(StarfallSalvage::gatePlateUnderPlayer({onGate(2, 1).x, onGate(2, 1).y,
                                             StarfallSalvage::playerWidth,
                                             StarfallSalvage::playerHeight}) == 4);

    EXPECT(StarfallSalvage::frameIntervalNs(0.5) == 80'000'000);
    EXPECT(StarfallSalvage::frameIntervalNs(1.0) == 40'000'000);
    EXPECT(StarfallSalvage::frameIntervalNs(2.0) == 20'000'000);
    EXPECT(StarfallSalvage::frameIntervalNs(0.0) == 40'000'000);

    const auto config = StarfallSalvage::serverConfig();
    EXPECT(config.platforms.size() == 5);
    EXPECT(config.spawnPoints.size() == 8);
    EXPECT(config.platforms[0].endX > config.platforms[0].startX);
    EXPECT(config.platforms[1].endX > config.platforms[1].startX);
    EXPECT(StarfallSalvage::isShuttle(config.platforms[0].id));
    EXPECT(StarfallSalvage::isDrone(config.platforms[2].id));
    EXPECT(StarfallSalvage::isDrone(config.platforms[3].id));
    EXPECT(StarfallSalvage::isDrone(config.platforms[4].id));
    EXPECT(config.platforms[2].endX - config.platforms[2].startX >= 300.0F);
    EXPECT(config.platforms[4].endX - config.platforms[4].startX >= 400.0F);
    for (const Rect& crate : StarfallSalvage::cargo) EXPECT(supportedByDeck(crate));
    for (const Rect& relay : StarfallSalvage::relayPads) EXPECT(supportedByDeck(relay));
    for (const Rect& plate : StarfallSalvage::gatePlates) EXPECT(supportedByDeck(plate));
    EXPECT(StarfallSalvage::bulkhead.x > StarfallSalvage::decks[3].x + StarfallSalvage::decks[3].width);
    EXPECT(StarfallSalvage::bulkhead.x + StarfallSalvage::bulkhead.width < StarfallSalvage::decks[4].x);
    EXPECT(StarfallSalvage::worldWidth > StarfallSalvage::width);
    EXPECT(StarfallSalvage::cameraTargetX(0.0F) == 0.0F);
    EXPECT(StarfallSalvage::cameraTargetX(800.0F) == 335.0F);
    EXPECT(StarfallSalvage::cameraTargetX(1550.0F) == 640.0F);
    EXPECT(StarfallSalvage::cameraTargetX(2000.0F) == 640.0F);

    std::cout << "Starfall Salvage cooperative rules passed\n";
}
