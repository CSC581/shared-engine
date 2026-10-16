// CPU cost and size of the client-server codecs, without any transport.
//
//   X  format     text (multipart) / binary (one frame)
//      message    snapshot  the server's reply to every POSITION, 8 players
//                 position  a client's POSITION request
//      platforms  10/100/500 (snapshot only; 0 for position)
//      data       opaque player data bytes 0/128/512
//   Y  encode and decode time per message (microseconds) and payload bytes
//
// The network benchmark measures these codecs inside the whole stack, where
// the socket round trip hides most of their cost. This one isolates the
// encode/decode work that runs on the server and every client per request.
// Bytes are frame contents only, the same convention as the network benchmark.

#include "BenchmarkSupport.hpp"
#include "BinaryNetworkProtocol.hpp"
#include "NetworkProtocol.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int players = 8;
const std::string token = "0123456789abcdef0123456789abcdef";

std::string playerData(int bytes, int seed)
{
    std::string data;
    for (int i = 0; i < bytes; ++i) {
        data.push_back(static_cast<char>('!' + (i * 7 + seed) % 90));
    }
    return data;
}

Network::Reply makeSnapshot(int platforms, int dataBytes)
{
    Network::Reply reply;
    reply.type = Network::ReplyType::Snapshot;
    reply.snapshot.serverTick = 123456;
    for (int i = 1; i <= players; ++i) {
        reply.snapshot.players.push_back(
            {static_cast<Network::PlayerId>(i), "player" + std::to_string(i), 100.25F * i, 48.5F * i,
             playerData(dataBytes, i)});
    }
    for (int i = 1; i <= platforms; ++i) {
        reply.snapshot.platforms.push_back(
            {static_cast<std::uint32_t>(i), 12.5F * i, 300.75F - i, 120.0F, 22.0F});
    }
    return reply;
}

Network::Request makePosition(int dataBytes)
{
    Network::Request request;
    request.type = Network::RequestType::Position;
    request.playerId = 3;
    request.sessionToken = token;
    request.position = {412.5F, 233.25F, 98765, playerData(dataBytes, 3)};
    return request;
}

std::size_t payloadBytes(const Network::Message& message)
{
    std::size_t bytes = 0;
    for (const std::string& frame : message) {
        bytes += frame.size();
    }
    return bytes;
}

// Times `iterations` calls of `work`, returning microseconds per call.
template <class Work>
double timePerCall(int iterations, Work&& work)
{
    const std::int64_t start = Bench::nowNs();
    for (int i = 0; i < iterations; ++i) {
        work();
    }
    return static_cast<double>(Bench::nowNs() - start) / iterations / 1e3;
}

void require(bool ok, const std::string& what, const std::string& error)
{
    if (!ok) {
        throw std::runtime_error(what + " failed to decode: " + error);
    }
}

// encode_us, decode_us, bytes
std::vector<double> measure(const std::string& format, const std::string& message, int platforms, int dataBytes,
                            int iterations)
{
    std::string error;
    if (message == "snapshot") {
        const Network::Reply reply = makeSnapshot(platforms, dataBytes);
        Network::Reply decoded;
        if (format == "text") {
            const Network::Message encoded = Network::encodeReply(reply);
            require(Network::decodeReply(encoded, decoded, error), "text snapshot", error);
            const double encodeUs = timePerCall(iterations, [&] { Bench::sink += Network::encodeReply(reply).size(); });
            const double decodeUs = timePerCall(iterations, [&] {
                Bench::sink += Network::decodeReply(encoded, decoded, error) ? 1U : 0U;
            });
            return {encodeUs, decodeUs, static_cast<double>(payloadBytes(encoded))};
        }
        const Network::BinaryMessage encoded = Network::encodeBinaryReply(reply);
        require(Network::decodeBinaryReply(encoded, decoded, error), "binary snapshot", error);
        const double encodeUs = timePerCall(iterations, [&] { Bench::sink += Network::encodeBinaryReply(reply).size(); });
        const double decodeUs = timePerCall(iterations, [&] {
            Bench::sink += Network::decodeBinaryReply(encoded, decoded, error) ? 1U : 0U;
        });
        return {encodeUs, decodeUs, static_cast<double>(encoded.size())};
    }

    const Network::Request request = makePosition(dataBytes);
    Network::Request decoded;
    if (format == "text") {
        const auto encode = [&] {
            return Network::encodePosition(request.playerId, request.sessionToken, request.position);
        };
        const Network::Message encoded = encode();
        require(Network::decodeRequest(encoded, decoded, error), "text position", error);
        const double encodeUs = timePerCall(iterations, [&] { Bench::sink += encode().size(); });
        const double decodeUs = timePerCall(iterations, [&] {
            Bench::sink += Network::decodeRequest(encoded, decoded, error) ? 1U : 0U;
        });
        return {encodeUs, decodeUs, static_cast<double>(payloadBytes(encoded))};
    }
    const Network::BinaryMessage encoded = Network::encodeBinaryRequest(request);
    require(Network::decodeBinaryRequest(encoded, decoded, error), "binary position", error);
    const double encodeUs = timePerCall(iterations, [&] { Bench::sink += Network::encodeBinaryRequest(request).size(); });
    const double decodeUs = timePerCall(iterations, [&] {
        Bench::sink += Network::decodeBinaryRequest(encoded, decoded, error) ? 1U : 0U;
    });
    return {encodeUs, decodeUs, static_cast<double>(encoded.size())};
}

} // namespace

int main(int argc, char** argv)
{
    const Bench::Options options = Bench::parseOptions(
        argc, argv, "codec-benchmark", "text versus binary encode/decode cost and size");

    const std::vector<std::string> formats{"text", "binary"};
    const std::vector<int> platformCounts = options.quick ? std::vector<int>{10} : std::vector<int>{10, 100, 500};
    const std::vector<int> dataSizes = options.quick ? std::vector<int>{0, 512} : std::vector<int>{0, 128, 512};
    const int repetitions = options.quick ? 1 : 5;
    const int iterations = options.quick ? 50 : 2000;

    Bench::Results results({"message", "platforms", "data_bytes", "format"},
                           {"encode_us", "decode_us", "payload_bytes"});

    try {
        for (int rep = 1; rep <= repetitions; ++rep) {
            for (int dataBytes : dataSizes) {
                for (const std::string& format : formats) {
                    results.add({"position", "0", std::to_string(dataBytes), format}, rep,
                                measure(format, "position", 0, dataBytes, iterations));
                }
                for (int platforms : platformCounts) {
                    for (const std::string& format : formats) {
                        results.add({"snapshot", std::to_string(platforms), std::to_string(dataBytes), format}, rep,
                                    measure(format, "snapshot", platforms, dataBytes, iterations));
                    }
                }
            }
            std::cout << "repetition " << rep << '/' << repetitions << " done\n";
        }
    } catch (const std::exception& error) {
        std::cerr << "codec-benchmark: " << error.what() << '\n';
        return 1;
    }

    results.write(options);
    results.print();
    return 0;
}
