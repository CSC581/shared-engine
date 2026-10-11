#include "NetworkClient.hpp"
#include "NetworkServerHost.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    int repetitions = 5;
    int warmupIterations = 100;
    int measuredIterations = 1000;
    std::string runsPath = "runs.csv";
    std::string latencyPath = "latency_samples.csv";
};

struct Cell {
    Net::DataFormat format;
    int clients;
    int platforms;
    int repetition;
};

struct ClientResult {
    Network::PlayerId playerId = 0;
    Network::TransportTelemetry before;
    Network::TransportTelemetry after;
    std::vector<std::int64_t> rttNs;
    std::uint64_t completed = 0;
    std::uint64_t objectChecks = 0;
    std::uint64_t objectFailures = 0;
    std::uint64_t disconnects = 0;
};

struct RunResult {
    bool success = false;
    std::string error;
    double elapsedSeconds = 0.0;
    std::uint64_t completed = 0;
    std::uint64_t objectChecks = 0;
    std::uint64_t objectFailures = 0;
    std::uint64_t disconnects = 0;
    std::uint64_t serverUpdates = 0;
    bool trafficMatch = false;
    Network::TransportTelemetry traffic;
    std::vector<ClientResult> clients;
};

class CancellableBarrier {
public:
    explicit CancellableBarrier(std::size_t participants)
        : participants_(participants)
    {
    }

    bool arriveAndWait()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (cancelled_) {
            return false;
        }
        const std::size_t generation = generation_;
        if (++arrived_ == participants_) {
            arrived_ = 0;
            ++generation_;
            condition_.notify_all();
            return true;
        }
        condition_.wait(lock, [&] { return cancelled_ || generation_ != generation; });
        return !cancelled_;
    }

    void cancel()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        cancelled_ = true;
        condition_.notify_all();
    }

private:
    const std::size_t participants_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t arrived_ = 0;
    std::size_t generation_ = 0;
    bool cancelled_ = false;
};

struct RunControl {
    explicit RunControl(std::size_t participants)
        : phases(participants + 1)
    {
    }

    void fail(std::string message)
    {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            if (!failed) {
                failed = true;
                error = std::move(message);
            }
        }
        phases.cancel();
        condition.notify_all();
    }

    CancellableBarrier phases;
    std::mutex mutex;
    std::condition_variable condition;
    bool failed = false;
    std::string error;
    std::size_t readyToMeasure = 0;
    std::size_t completedClients = 0;
    bool measure = false;
    bool shutdown = false;
    Clock::time_point finishedAt{};
};

const char* formatName(Net::DataFormat format)
{
    return format == Net::DataFormat::Binary ? "binary" : "text";
}

std::string csv(std::string value)
{
    if (value.find_first_of(",\"\r\n") == std::string::npos) {
        return value;
    }
    std::string escaped = "\"";
    for (char character : value) {
        if (character == '"') {
            escaped.push_back('"');
        }
        escaped.push_back(character);
    }
    escaped.push_back('"');
    return escaped;
}

Network::TransportTelemetry difference(const Network::TransportTelemetry& after,
                                       const Network::TransportTelemetry& before)
{
    const auto subtract = [](std::uint64_t newer, std::uint64_t older) {
        return newer >= older ? newer - older : 0;
    };
    return {subtract(after.messagesSent, before.messagesSent),
            subtract(after.messagesReceived, before.messagesReceived),
            subtract(after.framesSent, before.framesSent),
            subtract(after.framesReceived, before.framesReceived),
            subtract(after.payloadBytesSent, before.payloadBytesSent),
            subtract(after.payloadBytesReceived, before.payloadBytesReceived)};
}

void add(Network::TransportTelemetry& total, const Network::TransportTelemetry& value)
{
    total.messagesSent += value.messagesSent;
    total.messagesReceived += value.messagesReceived;
    total.framesSent += value.framesSent;
    total.framesReceived += value.framesReceived;
    total.payloadBytesSent += value.payloadBytesSent;
    total.payloadBytesReceived += value.payloadBytesReceived;
}

Network::ServerConfig serverConfig(int platformCount)
{
    Network::ServerConfig config;
    config.spawnPoints = {{0.0F, 0.0F}, {10.0F, 0.0F}, {20.0F, 0.0F}, {30.0F, 0.0F}};
    config.inactivityTimeoutTics = 60 * kNsPerSec;
    config.platforms.reserve(static_cast<std::size_t>(platformCount));
    for (int index = 0; index < platformCount; ++index) {
        const float x = static_cast<float>(index % 50) * 20.0F;
        const float y = static_cast<float>(index / 50) * 24.0F;
        Network::PlatformPath path;
        path.id = static_cast<std::uint32_t>(index + 1);
        path.startX = x;
        path.startY = y;
        path.endX = x + 100.0F + static_cast<float>(index % 7);
        path.endY = y;
        path.speed = 40.0F + static_cast<float>(index % 11);
        path.width = 48.0F;
        path.height = 12.0F;
        config.platforms.push_back(path);
    }
    return config;
}

Network::HostConfig hostConfig()
{
    Network::HostConfig config;
    config.mode = Network::HostMode::Dedicated;
    config.bindEndpoint = "tcp://127.0.0.1:*";
    config.advertiseHost = "127.0.0.1";
    config.tickInterval = std::chrono::milliseconds(1);
    config.pollInterval = std::chrono::milliseconds(5);
    return config;
}

bool completeRoundTrip(Network::NetworkClient& client, float x, float y,
                       const std::string& data, std::int64_t* elapsedNs,
                       std::string& error)
{
    const Clock::time_point begin = Clock::now();
    client.submitPosition(x, y, data);
    if (!client.awaitingReply()) {
        error = "client did not send a position request: " + client.error();
        return false;
    }

    const Clock::time_point deadline = begin + std::chrono::seconds(10);
    while (client.awaitingReply() && client.state() == Network::ConnectionState::Connected &&
           Clock::now() < deadline) {
        client.poll();
        std::this_thread::yield();
    }
    if (client.awaitingReply()) {
        error = "position request timed out";
        return false;
    }
    if (client.state() != Network::ConnectionState::Connected) {
        error = "client disconnected during a position request: " + client.error();
        return false;
    }
    if (elapsedNs != nullptr) {
        *elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
    }
    return true;
}

void runClient(std::size_t clientIndex, const Cell& cell, const Options& options,
               const std::string& endpoint, RunControl& control, ClientResult& result)
{
    RealTimeClock clock;
    Network::NetworkClient client(clock, endpoint, cell.format);
    client.setPlayerName("benchmark-" + std::to_string(clientIndex));
    client.start();

    const Clock::time_point connectDeadline = Clock::now() + std::chrono::seconds(10);
    while (client.state() == Network::ConnectionState::Connecting && Clock::now() < connectDeadline) {
        client.poll();
        std::this_thread::yield();
    }
    if (client.state() != Network::ConnectionState::Connected) {
        control.fail("client " + std::to_string(clientIndex) + " could not connect: " + client.error());
        return;
    }
    result.playerId = client.playerId();
    if (!control.phases.arriveAndWait()) {
        return;
    }

    // JOIN replies can contain an earlier roster. Drive ordinary position
    // round trips until every client sees the complete scene, outside timing.
    std::string transactionError;
    const Clock::time_point stableDeadline = Clock::now() + std::chrono::seconds(10);
    while ((client.snapshot().players.size() != static_cast<std::size_t>(cell.clients) ||
            client.snapshot().platforms.size() != static_cast<std::size_t>(cell.platforms)) &&
           Clock::now() < stableDeadline) {
        if (!completeRoundTrip(client, static_cast<float>(clientIndex), 0.0F, {}, nullptr,
                               transactionError)) {
            control.fail("client " + std::to_string(clientIndex) + " failed while stabilizing: " +
                         transactionError);
            return;
        }
    }
    if (client.snapshot().players.size() != static_cast<std::size_t>(cell.clients) ||
        client.snapshot().platforms.size() != static_cast<std::size_t>(cell.platforms)) {
        control.fail("client " + std::to_string(clientIndex) + " never received the complete scene");
        return;
    }
    if (!control.phases.arriveAndWait()) {
        return;
    }

    for (int iteration = 0; iteration < options.warmupIterations; ++iteration) {
        if (!completeRoundTrip(client, static_cast<float>(clientIndex), static_cast<float>(iteration),
                               "warmup", nullptr, transactionError)) {
            control.fail("client " + std::to_string(clientIndex) + " failed during warm-up: " +
                         transactionError);
            return;
        }
    }
    if (!control.phases.arriveAndWait()) {
        return;
    }

    result.before = client.telemetry();
    {
        std::unique_lock<std::mutex> lock(control.mutex);
        ++control.readyToMeasure;
        control.condition.notify_all();
        control.condition.wait(lock, [&] { return control.failed || control.measure; });
        if (control.failed) {
            return;
        }
    }

    result.rttNs.reserve(static_cast<std::size_t>(options.measuredIterations));
    for (int iteration = 0; iteration < options.measuredIterations; ++iteration) {
        std::int64_t elapsed = 0;
        if (!completeRoundTrip(client,
                               static_cast<float>(clientIndex * options.measuredIterations + iteration),
                               static_cast<float>(iteration), "measured-payload", &elapsed,
                               transactionError)) {
            ++result.disconnects;
            control.fail("client " + std::to_string(clientIndex) + " failed during measurement: " +
                         transactionError);
            return;
        }
        ++result.completed;
        result.rttNs.push_back(elapsed);
        ++result.objectChecks;
        if (client.snapshot().players.size() != static_cast<std::size_t>(cell.clients) ||
            client.snapshot().platforms.size() != static_cast<std::size_t>(cell.platforms)) {
            ++result.objectFailures;
        }
    }
    result.after = client.telemetry();

    {
        std::unique_lock<std::mutex> lock(control.mutex);
        ++control.completedClients;
        if (control.completedClients == static_cast<std::size_t>(cell.clients)) {
            control.finishedAt = Clock::now();
        }
        control.condition.notify_all();
        control.condition.wait(lock, [&] { return control.failed || control.shutdown; });
    }
    client.leave();
}

std::map<Network::PlayerId, std::uint64_t> trafficByPlayer(const Network::NetworkServerHost& host)
{
    std::map<Network::PlayerId, std::uint64_t> result;
    for (const Network::PlayerTraffic& player : host.traffic()) {
        result[player.id] = player.acceptedPositions;
    }
    return result;
}

RunResult runCell(const Cell& cell, const Options& options)
{
    Network::NetworkServerHost host(serverConfig(cell.platforms), hostConfig());
    host.start();

    RunResult result;
    result.clients.resize(static_cast<std::size_t>(cell.clients));
    RunControl control(static_cast<std::size_t>(cell.clients));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(cell.clients));
    for (int index = 0; index < cell.clients; ++index) {
        threads.emplace_back(runClient, static_cast<std::size_t>(index), std::cref(cell),
                             std::cref(options), host.boundEndpoint(), std::ref(control),
                             std::ref(result.clients[static_cast<std::size_t>(index)]));
    }

    if (control.phases.arriveAndWait()) {
        if (host.playerCount() != static_cast<std::size_t>(cell.clients) ||
            host.activeWorkers() != static_cast<std::size_t>(cell.clients)) {
            control.fail("server player or worker count did not match the client count");
        }
    }
    if (control.phases.arriveAndWait()) {
        // Complete-scene stabilization finished for every client.
    }
    if (control.phases.arriveAndWait()) {
        // Warm-up finished for every client.
    }

    Clock::time_point startedAt{};
    std::map<Network::PlayerId, std::uint64_t> beforeTraffic;
    {
        std::unique_lock<std::mutex> lock(control.mutex);
        control.condition.wait(lock, [&] {
            return control.failed || control.readyToMeasure == static_cast<std::size_t>(cell.clients);
        });
        if (!control.failed) {
            beforeTraffic = trafficByPlayer(host);
            startedAt = Clock::now();
            control.measure = true;
            control.condition.notify_all();
            control.condition.wait(lock, [&] {
                return control.failed || control.completedClients == static_cast<std::size_t>(cell.clients);
            });
        }
    }

    std::map<Network::PlayerId, std::uint64_t> afterTraffic;
    if (!control.failed) {
        afterTraffic = trafficByPlayer(host);
        result.elapsedSeconds = std::chrono::duration<double>(control.finishedAt - startedAt).count();
    }

    {
        const std::lock_guard<std::mutex> lock(control.mutex);
        control.shutdown = true;
        control.condition.notify_all();
    }
    for (std::thread& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    host.stop();

    {
        const std::lock_guard<std::mutex> lock(control.mutex);
        result.error = control.error;
    }
    const std::uint64_t expected = static_cast<std::uint64_t>(cell.clients) *
                                   static_cast<std::uint64_t>(options.measuredIterations);
    result.trafficMatch = !control.failed;
    for (const ClientResult& client : result.clients) {
        result.completed += client.completed;
        result.objectChecks += client.objectChecks;
        result.objectFailures += client.objectFailures;
        result.disconnects += client.disconnects;
        const Network::TransportTelemetry delta = difference(client.after, client.before);
        add(result.traffic, delta);

        const auto before = beforeTraffic.find(client.playerId);
        const auto after = afterTraffic.find(client.playerId);
        if (before == beforeTraffic.end() || after == afterTraffic.end() || after->second < before->second) {
            result.trafficMatch = false;
        } else {
            const std::uint64_t updates = after->second - before->second;
            result.serverUpdates += updates;
            if (updates != static_cast<std::uint64_t>(options.measuredIterations)) {
                result.trafficMatch = false;
            }
        }
    }

    const bool transportCountsMatch = result.traffic.messagesSent == expected &&
                                      result.traffic.messagesReceived == expected;
    const bool binaryFramesMatch = cell.format != Net::DataFormat::Binary ||
                                   (result.traffic.framesSent == expected &&
                                    result.traffic.framesReceived == expected);
    result.success = !control.failed && result.completed == expected &&
                     result.objectChecks == expected && result.objectFailures == 0 &&
                     result.disconnects == 0 && result.serverUpdates == expected &&
                     result.trafficMatch && transportCountsMatch && binaryFramesMatch;
    if (!result.success && result.error.empty()) {
        result.error = "correctness or traffic counters did not match expectations";
    }
    return result;
}

bool parsePositive(const std::string& text, int& value)
{
    try {
        std::size_t parsed = 0;
        const int candidate = std::stoi(text, &parsed);
        if (parsed != text.size() || candidate <= 0) {
            return false;
        }
        value = candidate;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void usage()
{
    std::cout <<
        "network-performance-benchmark [options]\n\n"
        "  --quick              1 repetition, 5 warm-up and 20 measured iterations\n"
        "  --repetitions N      repetitions per matrix cell (default 5)\n"
        "  --warmup N           warm-up iterations per client (default 100)\n"
        "  --iterations N       measured iterations per client (default 1000)\n"
        "  --runs PATH          aggregate CSV output (default runs.csv)\n"
        "  --latency PATH       per-round-trip CSV output (default latency_samples.csv)\n";
}

bool parseOptions(int argc, char* argv[], Options& options)
{
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            usage();
            return false;
        }
        if (argument == "--quick") {
            options.repetitions = 1;
            options.warmupIterations = 5;
            options.measuredIterations = 20;
            continue;
        }
        if (index + 1 >= argc) {
            std::cerr << argument << " requires a value\n";
            return false;
        }
        const std::string value = argv[++index];
        if (argument == "--repetitions") {
            if (!parsePositive(value, options.repetitions)) {
                return false;
            }
        } else if (argument == "--warmup") {
            if (!parsePositive(value, options.warmupIterations)) {
                return false;
            }
        } else if (argument == "--iterations") {
            if (!parsePositive(value, options.measuredIterations)) {
                return false;
            }
        } else if (argument == "--runs") {
            options.runsPath = value;
        } else if (argument == "--latency") {
            options.latencyPath = value;
        } else {
            std::cerr << "unknown option: " << argument << '\n';
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    Options options;
    if (!parseOptions(argc, argv, options)) {
        return argc > 1 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") ? 0 : 1;
    }

    std::ofstream runs(options.runsPath);
    std::ofstream latency(options.latencyPath);
    if (!runs || !latency) {
        std::cerr << "could not open benchmark output files\n";
        return 1;
    }
    runs << "run_id,format,clients,platforms,repetition,warmup_iterations,measured_iterations,"
            "elapsed_seconds,completed_round_trips,throughput_round_trips_per_second,rtt_sample_count,"
            "messages_sent,messages_received,frames_sent,frames_received,payload_bytes_sent,"
            "payload_bytes_received,object_count_checks,object_count_failures,disconnects,"
            "server_position_updates,expected_position_updates,traffic_match,success,error\n";
    latency << "run_id,format,clients,platforms,repetition,client_index,iteration,rtt_ns\n";
    runs << std::setprecision(12);

    const std::vector<Net::DataFormat> formats = {
        Net::DataFormat::TextMultipart,
        Net::DataFormat::Binary,
    };
    const std::vector<int> clientCounts = {1, 2, 4};
    const std::vector<int> platformCounts = {10, 100, 500};
    int runId = 0;
    int failures = 0;
    // Keep text/binary runs adjacent within each repetition so machine drift,
    // temperature and background load do not systematically favor the format
    // that happened to run second for the entire experiment.
    for (int repetition = 1; repetition <= options.repetitions; ++repetition) {
        for (int clients : clientCounts) {
            for (int platforms : platformCounts) {
                for (Net::DataFormat format : formats) {
                    const Cell cell{format, clients, platforms, repetition};
                    ++runId;
                    std::cout << "run " << runId << ": " << formatName(format) << ", " << clients
                              << " client(s), " << platforms << " platform(s), repetition " << repetition
                              << '/' << options.repetitions << std::endl;
                    const RunResult result = runCell(cell, options);
                    const std::uint64_t expected = static_cast<std::uint64_t>(clients) *
                                                   static_cast<std::uint64_t>(options.measuredIterations);
                    const double throughput = result.elapsedSeconds > 0.0
                                                  ? static_cast<double>(result.completed) / result.elapsedSeconds
                                                  : 0.0;
                    std::size_t samples = 0;
                    for (const ClientResult& client : result.clients) {
                        samples += client.rttNs.size();
                    }
                    runs << runId << ',' << formatName(format) << ',' << clients << ',' << platforms << ','
                         << repetition << ',' << options.warmupIterations << ',' << options.measuredIterations
                         << ',' << result.elapsedSeconds << ',' << result.completed << ',' << throughput << ','
                         << samples << ',' << result.traffic.messagesSent << ','
                         << result.traffic.messagesReceived << ',' << result.traffic.framesSent << ','
                         << result.traffic.framesReceived << ',' << result.traffic.payloadBytesSent << ','
                         << result.traffic.payloadBytesReceived << ',' << result.objectChecks << ','
                         << result.objectFailures << ',' << result.disconnects << ',' << result.serverUpdates
                         << ',' << expected << ',' << (result.trafficMatch ? 1 : 0) << ','
                         << (result.success ? 1 : 0) << ',' << csv(result.error) << '\n';
                    for (std::size_t clientIndex = 0; clientIndex < result.clients.size(); ++clientIndex) {
                        const std::vector<std::int64_t>& samplesForClient = result.clients[clientIndex].rttNs;
                        for (std::size_t iteration = 0; iteration < samplesForClient.size(); ++iteration) {
                            latency << runId << ',' << formatName(format) << ',' << clients << ',' << platforms
                                    << ',' << repetition << ',' << clientIndex << ',' << iteration << ','
                                    << samplesForClient[iteration] << '\n';
                        }
                    }
                    runs.flush();
                    latency.flush();
                    if (!result.success) {
                        ++failures;
                        std::cerr << "run " << runId << " failed: " << result.error << '\n';
                    }
                }
            }
        }
    }

    std::cout << "completed " << runId << " run(s), failures=" << failures
              << ", runs=" << options.runsPath << ", latency=" << options.latencyPath << '\n';
    return failures == 0 ? 0 : 1;
}
