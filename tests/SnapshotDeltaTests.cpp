#include "SnapshotDelta.hpp"

#include "WireFormat.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

bool expect(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "Test failed: " << message << '\n';
    }
    return condition;
}

std::uint8_t bit(Network::PlayerField field)
{
    return static_cast<std::uint8_t>(field);
}

std::uint8_t bit(Network::PlatformField field)
{
    return static_cast<std::uint8_t>(field);
}

bool sameFloat(float left, float right)
{
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::memcpy(&a, &left, sizeof(a));
    std::memcpy(&b, &right, sizeof(b));
    return a == b;
}

bool sameSnapshot(const Network::WorldSnapshot& left, const Network::WorldSnapshot& right)
{
    if (left.serverTick != right.serverTick || left.players.size() != right.players.size() ||
        left.platforms.size() != right.platforms.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.players.size(); ++i) {
        const auto& a = left.players[i];
        const auto& b = right.players[i];
        if (a.id != b.id || a.name != b.name || !sameFloat(a.x, b.x) ||
            !sameFloat(a.y, b.y) || a.data != b.data) {
            return false;
        }
    }
    for (std::size_t i = 0; i < left.platforms.size(); ++i) {
        const auto& a = left.platforms[i];
        const auto& b = right.platforms[i];
        if (a.id != b.id || !sameFloat(a.x, b.x) || !sameFloat(a.y, b.y) ||
            !sameFloat(a.width, b.width) || !sameFloat(a.height, b.height)) {
            return false;
        }
    }
    return true;
}

Network::WorldSnapshot baseline()
{
    return {10,
            {{2, "two", 2.0F, 20.0F, "old"}, {1, "one", -0.0F, 10.0F, {}}},
            {{8, 8.0F, 18.0F, 28.0F, 38.0F},
             {7, 7.0F, 17.0F, 27.0F, 37.0F}}};
}

bool roundTripAllChanges()
{
    const Network::WorldSnapshot before = baseline();
    Network::WorldSnapshot after{
        11,
        {{3, "three", 3.0F, 30.0F, "new"},
         {1, "renamed", 0.0F, 11.0F, std::string("raw\0data", 8)}},
        {{9, 9.0F, 19.0F, 29.0F, 39.0F},
         {7, 7.5F, 17.5F, 27.5F, 37.5F}}};

    const Network::SnapshotDelta delta = Network::makeSnapshotDelta(before, after);
    bool passed = true;
    passed &= expect(delta.addedPlayers.size() == 1 && delta.addedPlayers[0].id == 3,
                     "new player should be emitted as a full addition");
    passed &= expect(delta.removedPlayers.size() == 1 && delta.removedPlayers[0] == 2,
                     "missing player should be emitted as a removal");
    passed &= expect(delta.changedPlayers.size() == 1 &&
                         delta.changedPlayers[0].fields ==
                             (bit(Network::PlayerField::Name) | bit(Network::PlayerField::X) |
                              bit(Network::PlayerField::Y) | bit(Network::PlayerField::Data)),
                     "all changed player properties should be represented by the mask");
    passed &= expect(delta.addedPlatforms.size() == 1 && delta.addedPlatforms[0].id == 9 &&
                         delta.removedPlatforms.size() == 1 && delta.removedPlatforms[0] == 8,
                     "platform membership changes should be represented");
    passed &= expect(delta.changedPlatforms.size() == 1 &&
                         delta.changedPlatforms[0].fields == 0x0FU,
                     "all changed platform properties should be represented by the mask");

    Network::WorldSnapshot reconstructed;
    std::string error;
    passed &= expect(Network::applySnapshotDelta(before, delta, reconstructed, error),
                     "valid delta should apply: " + error);
    // makeSnapshotDelta and applySnapshotDelta canonicalize records by ID.
    Network::WorldSnapshot expected = after;
    std::sort(expected.players.begin(), expected.players.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });
    std::sort(expected.platforms.begin(), expected.platforms.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });
    passed &= expect(sameSnapshot(reconstructed, expected),
                     "applied delta should exactly reconstruct the target");
    passed &= expect(!sameFloat(before.players[1].x, reconstructed.players[0].x),
                     "bitwise float comparison should preserve a -0 to +0 change");
    return passed;
}

bool tickOnlyAndDeterministicOrder()
{
    Network::WorldSnapshot before = baseline();
    Network::WorldSnapshot after = before;
    after.serverTick = 12;
    std::reverse(after.players.begin(), after.players.end());
    std::reverse(after.platforms.begin(), after.platforms.end());

    const Network::SnapshotDelta delta = Network::makeSnapshotDelta(before, after);
    bool passed = expect(delta.addedPlayers.empty() && delta.removedPlayers.empty() &&
                             delta.changedPlayers.empty() && delta.addedPlatforms.empty() &&
                             delta.removedPlatforms.empty() && delta.changedPlatforms.empty(),
                         "reordering alone should produce an empty tick-advance delta");
    Network::WorldSnapshot reconstructed;
    std::string error;
    passed &= expect(Network::applySnapshotDelta(before, delta, reconstructed, error),
                     "empty delta should advance the tick");
    passed &= expect(reconstructed.players[0].id == 1 && reconstructed.platforms[0].id == 7,
                     "reconstructed records should use deterministic ID order");
    return passed;
}

bool individualPropertyMasks()
{
    bool passed = true;
    const Network::WorldSnapshot before = baseline();
    const auto playerCase = [&](auto mutate, Network::PlayerField expected, const std::string& label) {
        Network::WorldSnapshot after = before;
        after.serverTick = 11;
        mutate(after.players[0]);
        const auto delta = Network::makeSnapshotDelta(before, after);
        return expect(delta.changedPlayers.size() == 1 &&
                          delta.changedPlayers[0].fields == bit(expected),
                      label + " should set only its player mask bit");
    };
    passed &= playerCase([](auto& player) { player.name = "changed"; },
                         Network::PlayerField::Name, "name change");
    passed &= playerCase([](auto& player) { player.x += 1.0F; },
                         Network::PlayerField::X, "x change");
    passed &= playerCase([](auto& player) { player.y += 1.0F; },
                         Network::PlayerField::Y, "y change");
    passed &= playerCase([](auto& player) {
        player.data.assign(Net::maxPlayerDataLength, '\0');
    }, Network::PlayerField::Data, "opaque data change");

    const auto platformCase = [&](auto mutate, Network::PlatformField expected,
                                  const std::string& label) {
        Network::WorldSnapshot after = before;
        after.serverTick = 11;
        mutate(after.platforms[0]);
        const auto delta = Network::makeSnapshotDelta(before, after);
        return expect(delta.changedPlatforms.size() == 1 &&
                          delta.changedPlatforms[0].fields == bit(expected),
                      label + " should set only its platform mask bit");
    };
    passed &= platformCase([](auto& platform) { platform.x += 1.0F; },
                           Network::PlatformField::X, "platform x change");
    passed &= platformCase([](auto& platform) { platform.y += 1.0F; },
                           Network::PlatformField::Y, "platform y change");
    passed &= platformCase([](auto& platform) { platform.width += 1.0F; },
                           Network::PlatformField::Width, "platform width change");
    passed &= platformCase([](auto& platform) { platform.height += 1.0F; },
                           Network::PlatformField::Height, "platform height change");
    return passed;
}

bool intermediateTraceReplay()
{
    Network::WorldSnapshot authoritative = baseline();
    Network::WorldSnapshot replica = authoritative;
    bool passed = true;
    for (int step = 1; step <= 180; ++step) {
        const Network::WorldSnapshot before = authoritative;
        ++authoritative.serverTick;
        authoritative.players[0].x += 0.5F;
        authoritative.players[1].y += step % 2 == 0 ? 0.25F : -0.25F;
        for (auto& platform : authoritative.platforms) {
            platform.x += 0.125F;
        }
        if (step % 60 == 0) {
            authoritative.players[0].data = "frame-" + std::to_string(step);
        }
        if (step == 75) {
            authoritative.players.push_back({3, "three", 3.0F, 30.0F, "joined"});
        }
        if (step == 125) {
            authoritative.players.erase(
                std::remove_if(authoritative.players.begin(), authoritative.players.end(),
                               [](const auto& player) { return player.id == 1; }),
                authoritative.players.end());
        }

        const auto delta = Network::makeSnapshotDelta(before, authoritative);
        std::string error;
        Network::WorldSnapshot next;
        if (!Network::applySnapshotDelta(replica, delta, next, error)) {
            passed &= expect(false, "trace delta should apply at step " + std::to_string(step) +
                                        ": " + error);
            break;
        }
        replica = std::move(next);
        Network::WorldSnapshot expected = authoritative;
        std::sort(expected.players.begin(), expected.players.end(),
                  [](const auto& a, const auto& b) { return a.id < b.id; });
        std::sort(expected.platforms.begin(), expected.platforms.end(),
                  [](const auto& a, const auto& b) { return a.id < b.id; });
        if (!sameSnapshot(replica, expected)) {
            passed &= expect(false, "trace should reconstruct exactly at step " +
                                        std::to_string(step));
            break;
        }
    }
    return passed;
}

template <typename Mutate>
bool rejectedWithoutMutation(const Network::SnapshotDelta& original, Mutate mutate,
                             const std::string& description)
{
    Network::SnapshotDelta invalid = original;
    mutate(invalid);
    Network::WorldSnapshot sentinel{999, {{99, "sentinel", 1, 2, {}}}, {}};
    const Network::WorldSnapshot unchanged = sentinel;
    std::string error;
    const bool accepted = Network::applySnapshotDelta(baseline(), invalid, sentinel, error);
    return expect(!accepted && !error.empty(), description + " should be rejected") &&
           expect(sameSnapshot(sentinel, unchanged),
                  description + " must not partially mutate the output");
}

bool malformedDeltasAreTransactional()
{
    Network::WorldSnapshot target = baseline();
    target.serverTick = 11;
    target.players[0].x = 9.0F;
    target.platforms[0].width = 29.0F;
    const Network::SnapshotDelta valid = Network::makeSnapshotDelta(baseline(), target);

    bool passed = true;
    passed &= rejectedWithoutMutation(valid, [](auto& d) { d.baseTick = 9; }, "baseline mismatch");
    passed &= rejectedWithoutMutation(valid, [](auto& d) { d.targetTick = d.baseTick; },
                                       "non-advancing target tick");
    passed &= rejectedWithoutMutation(valid, [](auto& d) { d.targetPlayerCount += 1; },
                                       "target count mismatch");
    passed &= rejectedWithoutMutation(valid, [](auto& d) { d.changedPlayers[0].fields = 0; },
                                       "zero player mask");
    passed &= rejectedWithoutMutation(valid, [](auto& d) { d.changedPlayers[0].fields = 0x80U; },
                                       "unknown player mask");
    passed &= rejectedWithoutMutation(valid, [](auto& d) {
        d.removedPlayers.push_back(d.changedPlayers[0].id);
    }, "player repeated across sections");
    passed &= rejectedWithoutMutation(valid, [](auto& d) {
        d.changedPlayers[0].fields = bit(Network::PlayerField::X);
        d.changedPlayers[0].x = std::numeric_limits<float>::infinity();
    }, "non-finite player property");
    passed &= rejectedWithoutMutation(valid, [](auto& d) {
        d.changedPlatforms[0].fields = bit(Network::PlatformField::Width);
        d.changedPlatforms[0].width = 0.0F;
    }, "invalid platform size");
    passed &= rejectedWithoutMutation(valid, [](auto& d) {
        d.addedPlayers.push_back({4, std::string(Net::maxNameLength + 1, 'x'), 1, 2, {}});
        d.targetPlayerCount += 1;
    }, "oversized player name");
    return passed;
}

bool invalidSnapshotsAreRejected()
{
    bool passed = true;
    Network::WorldSnapshot target = baseline();
    target.serverTick = 11;
    target.players.push_back(target.players.front());
    try {
        (void)Network::makeSnapshotDelta(baseline(), target);
        passed &= expect(false, "duplicate target player should throw");
    } catch (const std::invalid_argument&) {
    }

    target = baseline();
    target.serverTick = 10;
    try {
        (void)Network::makeSnapshotDelta(baseline(), target);
        passed &= expect(false, "non-advancing target snapshot should throw");
    } catch (const std::invalid_argument&) {
    }
    return passed;
}

} // namespace

int main()
{
    bool passed = true;
    passed &= roundTripAllChanges();
    passed &= tickOnlyAndDeterministicOrder();
    passed &= individualPropertyMasks();
    passed &= intermediateTraceReplay();
    passed &= malformedDeltasAreTransactional();
    passed &= invalidSnapshotsAreRejected();
    if (passed) {
        std::cout << "Snapshot delta tests passed\n";
    }
    return passed ? 0 : 1;
}
