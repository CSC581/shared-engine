// Feasibility comparison for full binary snapshots versus property deltas.
//
// Full snapshots use the production binary codec. The delta side models a
// concrete binary packet and replays typed changes, but is deliberately not a
// production protocol. It answers whether the payload savings justify adding
// baseline/recovery semantics to the real network stack.

#include "BenchmarkSupport.hpp"
#include "BinaryNetworkProtocol.hpp"
#include "NetworkProtocol.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr std::uint8_t xChanged = 1U << 0U;
constexpr std::uint8_t yChanged = 1U << 1U;
constexpr std::uint8_t dataChanged = 1U << 2U;
constexpr std::uint8_t widthChanged = 1U << 2U;
constexpr std::uint8_t heightChanged = 1U << 3U;

struct PlayerDelta {
    Network::PlayerId id = 0;
    std::uint8_t mask = 0;
    float x = 0.0F;
    float y = 0.0F;
    std::string data;
};

struct PlatformDelta {
    std::uint32_t id = 0;
    std::uint8_t mask = 0;
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
};

struct Delta {
    std::uint64_t baseTick = 0;
    std::uint64_t newTick = 0;
    std::vector<PlayerDelta> players;
    std::vector<PlatformDelta> platforms;
    std::size_t modeledBytes = 0;
    std::size_t changedProperties = 0;
};

bool sameSnapshot(const Network::WorldSnapshot& left, const Network::WorldSnapshot& right)
{
    if (left.serverTick != right.serverTick || left.players.size() != right.players.size() ||
        left.platforms.size() != right.platforms.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.players.size(); ++i) {
        const auto& a = left.players[i];
        const auto& b = right.players[i];
        if (a.id != b.id || a.name != b.name || a.x != b.x || a.y != b.y || a.data != b.data) {
            return false;
        }
    }
    for (std::size_t i = 0; i < left.platforms.size(); ++i) {
        const auto& a = left.platforms[i];
        const auto& b = right.platforms[i];
        if (a.id != b.id || a.x != b.x || a.y != b.y || a.width != b.width || a.height != b.height) {
            return false;
        }
    }
    return true;
}

std::string playerData(int bytes, int seed)
{
    std::string data(static_cast<std::size_t>(bytes), 'a');
    for (int i = 0; i < bytes; ++i) {
        data[static_cast<std::size_t>(i)] = static_cast<char>('!' + (i + seed) % 90);
    }
    return data;
}

Network::WorldSnapshot initialSnapshot(int players, int platforms, int dataBytes)
{
    Network::WorldSnapshot snapshot;
    snapshot.serverTick = 1;
    for (int i = 0; i < players; ++i) {
        snapshot.players.push_back({static_cast<Network::PlayerId>(i + 1), "player" + std::to_string(i + 1),
                                    20.0F * i, 40.0F, playerData(dataBytes, i)});
    }
    for (int i = 0; i < platforms; ++i) {
        snapshot.platforms.push_back({static_cast<std::uint32_t>(i + 1), 12.0F * i, 100.0F,
                                      48.0F, 12.0F});
    }
    return snapshot;
}

void advance(Network::WorldSnapshot& snapshot, const std::string& workload, int iteration)
{
    ++snapshot.serverTick;
    const bool movePlayers = workload == "player-only" || workload == "all-moving";
    const bool movePlatforms = workload == "platforms-only" || workload == "all-moving";

    if (movePlayers) {
        const std::size_t count = workload == "player-only" ? 1U : snapshot.players.size();
        for (std::size_t i = 0; i < count; ++i) {
            snapshot.players[i].x += 0.5F;
            snapshot.players[i].y += (iteration % 2 == 0 ? 0.25F : -0.25F);
        }
        if (!snapshot.players.empty() && !snapshot.players[0].data.empty() && iteration % 60 == 0) {
            snapshot.players[0].data[0] = snapshot.players[0].data[0] == '!' ? '"' : '!';
        }
    }
    if (movePlatforms) {
        for (Network::PlatformState& platform : snapshot.platforms) {
            platform.x += 0.25F;
            platform.y += (iteration % 2 == 0 ? 0.125F : -0.125F);
        }
    }
}

Delta makeDelta(const Network::WorldSnapshot& before, const Network::WorldSnapshot& after)
{
    if (before.players.size() != after.players.size() || before.platforms.size() != after.platforms.size()) {
        throw std::runtime_error("feasibility delta does not model membership changes");
    }

    Delta delta;
    delta.baseTick = before.serverTick;
    delta.newTick = after.serverTick;
    delta.players.reserve(after.players.size());
    delta.platforms.reserve(after.platforms.size());
    // Production-like 8-byte header, two ticks and two record counts.
    delta.modeledBytes = 8U + 8U + 8U + 4U + 4U;

    for (std::size_t i = 0; i < before.players.size(); ++i) {
        const auto& oldPlayer = before.players[i];
        const auto& newPlayer = after.players[i];
        if (oldPlayer.id != newPlayer.id || oldPlayer.name != newPlayer.name) {
            throw std::runtime_error("feasibility delta requires stable player identity and name");
        }
        PlayerDelta change;
        change.id = newPlayer.id;
        if (oldPlayer.x != newPlayer.x) {
            change.mask |= xChanged;
            change.x = newPlayer.x;
            delta.modeledBytes += 4U;
            ++delta.changedProperties;
        }
        if (oldPlayer.y != newPlayer.y) {
            change.mask |= yChanged;
            change.y = newPlayer.y;
            delta.modeledBytes += 4U;
            ++delta.changedProperties;
        }
        if (oldPlayer.data != newPlayer.data) {
            change.mask |= dataChanged;
            change.data = newPlayer.data;
            delta.modeledBytes += 2U + change.data.size();
            ++delta.changedProperties;
        }
        if (change.mask != 0) {
            delta.modeledBytes += 4U + 1U; // id and property mask
            delta.players.push_back(std::move(change));
        }
    }

    for (std::size_t i = 0; i < before.platforms.size(); ++i) {
        const auto& oldPlatform = before.platforms[i];
        const auto& newPlatform = after.platforms[i];
        if (oldPlatform.id != newPlatform.id) {
            throw std::runtime_error("feasibility delta requires stable platform identity");
        }
        PlatformDelta change;
        change.id = newPlatform.id;
        const auto property = [&](float oldValue, float newValue, std::uint8_t bit, float& destination) {
            if (oldValue != newValue) {
                change.mask |= bit;
                destination = newValue;
                delta.modeledBytes += 4U;
                ++delta.changedProperties;
            }
        };
        property(oldPlatform.x, newPlatform.x, xChanged, change.x);
        property(oldPlatform.y, newPlatform.y, yChanged, change.y);
        property(oldPlatform.width, newPlatform.width, widthChanged, change.width);
        property(oldPlatform.height, newPlatform.height, heightChanged, change.height);
        if (change.mask != 0) {
            delta.modeledBytes += 4U + 1U;
            delta.platforms.push_back(change);
        }
    }
    return delta;
}

class Replica {
public:
    explicit Replica(Network::WorldSnapshot baseline)
        : snapshot_(std::move(baseline))
    {
        for (std::size_t i = 0; i < snapshot_.players.size(); ++i) {
            players_[snapshot_.players[i].id] = i;
        }
        for (std::size_t i = 0; i < snapshot_.platforms.size(); ++i) {
            platforms_[snapshot_.platforms[i].id] = i;
        }
    }

    void apply(const Delta& delta)
    {
        if (snapshot_.serverTick != delta.baseTick) {
            throw std::runtime_error("delta baseline tick mismatch");
        }
        for (const PlayerDelta& change : delta.players) {
            Network::PlayerState& player = snapshot_.players.at(players_.at(change.id));
            if ((change.mask & xChanged) != 0) player.x = change.x;
            if ((change.mask & yChanged) != 0) player.y = change.y;
            if ((change.mask & dataChanged) != 0) player.data = change.data;
        }
        for (const PlatformDelta& change : delta.platforms) {
            Network::PlatformState& platform = snapshot_.platforms.at(platforms_.at(change.id));
            if ((change.mask & xChanged) != 0) platform.x = change.x;
            if ((change.mask & yChanged) != 0) platform.y = change.y;
            if ((change.mask & widthChanged) != 0) platform.width = change.width;
            if ((change.mask & heightChanged) != 0) platform.height = change.height;
        }
        snapshot_.serverTick = delta.newTick;
    }

    const Network::WorldSnapshot& snapshot() const { return snapshot_; }

private:
    Network::WorldSnapshot snapshot_;
    std::unordered_map<Network::PlayerId, std::size_t> players_;
    std::unordered_map<std::uint32_t, std::size_t> platforms_;
};

std::vector<double> measure(int players, int platforms, int dataBytes, const std::string& workload,
                            int iterations)
{
    std::vector<Network::WorldSnapshot> trace;
    trace.reserve(static_cast<std::size_t>(iterations) + 1U);
    trace.push_back(initialSnapshot(players, platforms, dataBytes));
    for (int iteration = 0; iteration < iterations; ++iteration) {
        trace.push_back(trace.back());
        advance(trace.back(), workload, iteration);
    }

    Network::Reply baselineReply;
    baselineReply.type = Network::ReplyType::Snapshot;
    baselineReply.snapshot = trace.front();
    const double baselineBytes = static_cast<double>(Network::encodeBinaryReply(baselineReply).size());

    std::vector<Network::Reply> fullReplies;
    fullReplies.reserve(static_cast<std::size_t>(iterations));
    for (int iteration = 1; iteration <= iterations; ++iteration) {
        Network::Reply reply;
        reply.type = Network::ReplyType::Snapshot;
        reply.snapshot = trace[static_cast<std::size_t>(iteration)];
        fullReplies.push_back(std::move(reply));
    }

    std::vector<Network::BinaryMessage> fullMessages;
    fullMessages.reserve(static_cast<std::size_t>(iterations));
    std::size_t fullBytes = 0;
    const std::int64_t fullEncodeStart = Bench::nowNs();
    for (const Network::Reply& reply : fullReplies) {
        fullMessages.push_back(Network::encodeBinaryReply(reply));
        fullBytes += fullMessages.back().size();
    }
    const double fullEncodeUs = static_cast<double>(Bench::nowNs() - fullEncodeStart) / iterations / 1e3;

    Network::Reply decoded;
    std::string error;
    const std::int64_t fullDecodeStart = Bench::nowNs();
    for (const auto& message : fullMessages) {
        if (!Network::decodeBinaryReply(message, decoded, error)) {
            throw std::runtime_error("production full snapshot failed to decode: " + error);
        }
    }
    const double fullDecodeUs = static_cast<double>(Bench::nowNs() - fullDecodeStart) / iterations / 1e3;
    if (!sameSnapshot(decoded.snapshot, trace.back())) {
        throw std::runtime_error("production full snapshot replay diverged");
    }

    std::vector<Delta> deltas;
    deltas.reserve(static_cast<std::size_t>(iterations));
    std::size_t deltaBytes = 0;
    std::size_t changedProperties = 0;
    const std::int64_t deltaBuildStart = Bench::nowNs();
    for (int iteration = 1; iteration <= iterations; ++iteration) {
        deltas.push_back(makeDelta(trace[static_cast<std::size_t>(iteration - 1)],
                                   trace[static_cast<std::size_t>(iteration)]));
        deltaBytes += deltas.back().modeledBytes;
        changedProperties += deltas.back().changedProperties;
    }
    const double deltaBuildUs = static_cast<double>(Bench::nowNs() - deltaBuildStart) / iterations / 1e3;

    Replica replica(trace.front());
    const std::int64_t deltaApplyStart = Bench::nowNs();
    for (const Delta& delta : deltas) {
        replica.apply(delta);
    }
    const double deltaApplyUs = static_cast<double>(Bench::nowNs() - deltaApplyStart) / iterations / 1e3;
    if (!sameSnapshot(replica.snapshot(), trace.back())) {
        throw std::runtime_error("property-delta replay diverged");
    }

    Replica checked(trace.front());
    for (int iteration = 0; iteration < iterations; ++iteration) {
        checked.apply(deltas[static_cast<std::size_t>(iteration)]);
        if (!sameSnapshot(checked.snapshot(), trace[static_cast<std::size_t>(iteration + 1)])) {
            throw std::runtime_error("property-delta intermediate state diverged");
        }
    }

    const double fullPerUpdate = static_cast<double>(fullBytes) / iterations;
    const double deltaPerUpdate = static_cast<double>(deltaBytes) / iterations;
    return {fullEncodeUs, fullDecodeUs, deltaBuildUs, deltaApplyUs, baselineBytes,
            fullPerUpdate, deltaPerUpdate, 1.0 - deltaPerUpdate / fullPerUpdate,
            static_cast<double>(changedProperties) / iterations};
}

} // namespace

int main(int argc, char** argv)
{
    const Bench::Options options = Bench::parseOptions(
        argc, argv, "snapshot-strategy-benchmark", "full binary snapshots versus modeled property deltas");
    const std::vector<int> playerCounts = options.quick ? std::vector<int>{2} : std::vector<int>{1, 2, 4};
    const std::vector<int> platformCounts = options.quick ? std::vector<int>{10} : std::vector<int>{10, 100, 500};
    const std::vector<int> dataSizes = options.quick ? std::vector<int>{128} : std::vector<int>{0, 128, 512};
    const std::vector<std::string> workloads{"player-only", "platforms-only", "all-moving"};
    const int repetitions = options.quick ? 1 : 5;
    const int iterations = options.quick ? 20 : 1000;

    Bench::Results results({"workload", "players", "platforms", "data_bytes"},
                           {"full_encode_us", "full_decode_us", "delta_build_us", "delta_apply_us",
                            "baseline_bytes", "full_bytes_per_update", "modeled_delta_bytes_per_update",
                            "modeled_payload_reduction", "changed_properties_per_update"});
    try {
        for (int repetition = 1; repetition <= repetitions; ++repetition) {
            for (int players : playerCounts) {
                for (int platforms : platformCounts) {
                    for (int dataBytes : dataSizes) {
                        for (const std::string& workload : workloads) {
                            results.add({workload, std::to_string(players), std::to_string(platforms),
                                         std::to_string(dataBytes)}, repetition,
                                        measure(players, platforms, dataBytes, workload, iterations));
                        }
                    }
                }
            }
            std::cout << "repetition " << repetition << '/' << repetitions << " done\n";
        }
        results.write(options);
        results.print();
    } catch (const std::exception& error) {
        std::cerr << "snapshot-strategy-benchmark: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
