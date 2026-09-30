// Headless server for Pokemon Hunt. It owns the boss (Mewtwo) and its shadow
// balls; the Pokemon (players) belong to their clients.
//
//   ./build/pokemon-hunt-server                     # tcp://*:5600
//   ./build/pokemon-hunt-server --advertise 192.168.1.10
//
// Same design as the engine's network-server (sandbox/NetworkServerMain.cpp):
// a JOIN handshake, one blocking REP worker thread per client (no
// Router/Dealer), and a thread moving world objects on real time. Kept as a
// game-local copy so the shared sandbox stays untouched.
//
// The engine's NetworkServer only knows ping-pong platforms, and a boss has to
// walk, jump and attack. So this server runs its own Boss on real time and
// puts the boss and its shadow balls into every snapshot in place of the
// platforms. Clients see them through Multiplayer::Session::platforms() in
// both client-server and peer-to-peer mode. The boss and its shadow balls are
// engine Entities, moved by the engine's Physics (SDL-free engine-geometry).
#include "Collision.hpp"
#include "Entity.hpp"
#include "NetworkServer.hpp"
#include "Physics.hpp"
#include "NetworkProtocol.hpp"
#include "PokemonHuntTransport.hpp"
#include "PokemonHuntWorld.hpp"
#include "TimeSource.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{

    using namespace PokemonHunt;

    // ---------------------------------------------------------------------------
    // Tuning
    // ---------------------------------------------------------------------------
    // Shadow balls leave the boss this far above the floor.
    constexpr float shadowBallLaunchHeight = 70.0F;
    constexpr float bossChargeSeconds = 0.7F;
    constexpr float bossFirstActionDelay = 2.0F;
    constexpr float bossMinActionDelay = 1.2F;
    constexpr float bossMaxActionDelay = 2.8F;
    // Every action rolls 0..actionRollSides-1: below jumpRolls it jumps, below
    // jumpRolls + chargeRolls it charges a shadow ball, otherwise it turns around.
    constexpr int actionRollSides = 10;
    constexpr int jumpRolls = 4;
    constexpr int chargeRolls = 4;
    // Shadow ball ids wrap after this many, so they never run into other ids.
    constexpr std::uint32_t shadowBallIdRange = 10000;
    // Shadow balls live while they overlap the screen.
    constexpr Rect screenBounds{0.0F, 0.0F, static_cast<float>(windowWidth), static_cast<float>(windowHeight)};

    constexpr auto worldTickInterval = std::chrono::milliseconds(16);
    // Longest step the world takes at once, so a stall does not teleport the boss.
    constexpr float maxWorldStep = 0.05F;
    // Blocking sockets wake this often so shutdown can reach them.
    constexpr auto socketReceiveTimeout = std::chrono::milliseconds(500);

    constexpr const char *defaultAdvertiseHost = "127.0.0.1";
    constexpr const char *workerBindEndpoint = "tcp://0.0.0.0:*";

    std::atomic<bool> g_running{true};

    void onSignal(int)
    {
        g_running.store(false);
    }

    // ---------------------------------------------------------------------------
    // The boss
    // ---------------------------------------------------------------------------
    // Mewtwo, Bowser style: walks back and forth across the floor, and every few
    // seconds either jumps or stops to charge up and fire a shadow ball. It owns
    // its HP: when that runs out it vanishes, and drops back in at a random spot
    // bossRespawnSeconds later.
    // Thread-safe: the world thread advances it while client workers read it.
    class Boss
    {
    public:
        // Physics gravity is one global value; in this process only the boss falls.
        Boss() { Physics::setGravity(bossGravity); }

        void update(float dt)
        {
            const std::lock_guard<std::mutex> lock(mutex_);

            if (fainted_)
            {
                updateFainted(dt);
                return;
            }

            if (state_ == State::Charging)
            {
                updateCharging(dt);
            }
            else
            {
                move(dt);
                if (state_ == State::Jumping)
                {
                    land();
                }
                else
                {
                    updateWalking(dt);
                }
            }
            moveBalls(dt);
        }

        // `damage` is the running total one Pokemon reports with every position;
        // only what is new since its last report comes off the HP. Damage that
        // lands while the boss is fainted is spent on nothing.
        void reportDamage(Network::PlayerId player, long long damage)
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            long long &last = reported_[player];
            const long long fresh = damage - last;
            if (fresh <= 0)
            {
                return;
            }
            last = damage;
            if (fainted_)
            {
                return;
            }
            hp_ -= static_cast<int>(std::min<long long>(fresh, hp_));
            if (hp_ <= 0)
            {
                faint();
            }
        }

        std::vector<Network::PlatformState> objects() const
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            std::vector<Network::PlatformState> result;
            result.push_back(statusObject());
            if (fainted_)
            {
                return result;
            }

            result.push_back(toPlatform(bossIdBase + flags(), body_));
            for (const Ball &ball : balls_)
            {
                result.push_back(toPlatform(ball.id, ball.body));
            }
            return result;
        }

    private:
        enum class State
        {
            Walking,
            Jumping,
            Charging
        };

        struct Ball
        {
            std::uint32_t id;
            Entity body;
        };

        static Network::PlatformState toPlatform(std::uint32_t id, const Entity &entity)
        {
            const Rect bounds = entity.getBounds();
            return {id, bounds.x, bounds.y, bounds.width, bounds.height};
        }

        float direction() const { return facingLeft_ ? -1.0F : 1.0F; }

        // --- update() steps; the caller holds mutex_. ----------------------------
        void updateFainted(float dt)
        {
            respawnIn_ -= dt;
            if (respawnIn_ <= 0.0F)
            {
                respawn();
            }
        }

        void updateCharging(float dt)
        {
            timer_ -= dt;
            if (timer_ <= 0.0F)
            {
                fireShadowBall();
                state_ = State::Walking;
                timer_ = nextActionDelay();
            }
        }

        // Walks on (falling too, mid-jump), turning around at either edge of the screen.
        void move(float dt)
        {
            body_.setVelocityX(direction() * bossWalkSpeed);
            if (state_ == State::Jumping)
            {
                Physics::applyGravity(body_, dt);
            }
            body_.update(dt);

            const float x = body_.getX();
            if (x <= bossMinX)
            {
                body_.setPosition(bossMinX, body_.getY());
                facingLeft_ = false;
            }
            else if (x >= bossMaxX)
            {
                body_.setPosition(bossMaxX, body_.getY());
                facingLeft_ = true;
            }
        }

        // Ends a jump once the boss is back on the floor.
        void land()
        {
            if (body_.getY() >= groundY - bossHeight)
            {
                body_.setPosition(body_.getX(), groundY - bossHeight);
                body_.setVelocityY(0.0F);
                state_ = State::Walking;
            }
        }

        void updateWalking(float dt)
        {
            timer_ -= dt;
            if (timer_ <= 0.0F)
            {
                startAction();
            }
        }

        void fireShadowBall()
        {
            const float x = body_.getX();
            const float startX = facingLeft_ ? x - shadowBallSize : x + bossWidth;
            Entity ball(startX, groundY - shadowBallLaunchHeight, shadowBallSize, shadowBallSize);
            ball.setVelocityX(direction() * shadowBallSpeed);
            balls_.push_back({nextBallId_++, std::move(ball)});
        }

        void moveBalls(float dt)
        {
            for (Ball &ball : balls_)
            {
                ball.body.update(dt);
            }
            balls_.erase(std::remove_if(balls_.begin(), balls_.end(),
                                        [](const Ball &ball)
                                        {
                                            return !Collision::intersects(ball.body.getBounds(), screenBounds);
                                        }),
                         balls_.end());
            if (nextBallId_ > shadowBallIdBase + shadowBallIdRange)
            {
                nextBallId_ = shadowBallIdBase;
            }
        }

        void startAction()
        {
            const int roll = std::uniform_int_distribution<int>(0, actionRollSides - 1)(random_);
            if (roll < jumpRolls)
            {
                state_ = State::Jumping;
                body_.setVelocityY(-bossJumpSpeed);
            }
            else if (roll < jumpRolls + chargeRolls)
            {
                state_ = State::Charging;
                timer_ = bossChargeSeconds;
            }
            else
            {
                facingLeft_ = !facingLeft_;
                timer_ = nextActionDelay();
            }
        }

        float nextActionDelay()
        {
            return std::uniform_real_distribution<float>(bossMinActionDelay, bossMaxActionDelay)(random_);
        }

        void faint()
        {
            fainted_ = true;
            respawnIn_ = bossRespawnSeconds;
            ++knockouts_;
            balls_.clear();
            std::cout << "Mewtwo fainted (KO " << knockouts_ << "); back in " << bossRespawnSeconds << "s\n";
        }

        // Back at full health, dropping in from above the screen at a random x.
        void respawn()
        {
            fainted_ = false;
            hp_ = bossMaxHp;
            body_.setPosition(std::uniform_real_distribution<float>(bossMinX, bossMaxX)(random_), -bossHeight);
            body_.setVelocity(0.0F, 0.0F);
            facingLeft_ = std::uniform_int_distribution<int>(0, 1)(random_) == 0;
            state_ = State::Jumping;
            timer_ = nextActionDelay();
            std::cout << "Mewtwo is back at x=" << static_cast<int>(body_.getX()) << '\n';
        }

        // --- objects() pieces; the caller holds mutex_. --------------------------
        // See bossStatusId for how the fields are packed.
        Network::PlatformState statusObject() const
        {
            const float respawnIn = fainted_ ? std::max(respawnIn_, 0.0F) : 0.0F;
            return {bossStatusId, static_cast<float>(hp_), respawnIn, static_cast<float>(knockouts_ + 1), 1.0F};
        }

        std::uint32_t flags() const
        {
            std::uint32_t result = 0;
            if (facingLeft_)
            {
                result |= bossFacingLeft;
            }
            if (state_ == State::Charging)
            {
                result |= bossCharging;
            }
            return result;
        }

        mutable std::mutex mutex_;
        std::mt19937 random_{std::random_device{}()};
        State state_ = State::Walking;
        Entity body_{(windowWidth - bossWidth) * 0.5F, groundY - bossHeight, bossWidth, bossHeight};
        bool facingLeft_ = true;
        float timer_ = bossFirstActionDelay;
        std::vector<Ball> balls_;
        std::uint32_t nextBallId_ = shadowBallIdBase;
        int hp_ = bossMaxHp;
        bool fainted_ = false;
        float respawnIn_ = 0.0F;
        long long knockouts_ = 0;
        std::unordered_map<Network::PlayerId, long long> reported_;
    };

    // ---------------------------------------------------------------------------
    // Requests
    // ---------------------------------------------------------------------------
    // Lets the engine server handle the request, then puts this game in: the
    // damage the Pokemon reported comes off the boss, and the reply carries the
    // boss in place of the engine's (empty) platform list. Damage is only read
    // from a position the engine server accepted, so a request with somebody
    // else's player id or a stale sequence cannot hurt the boss.
    Network::Message handleWithBoss(Network::NetworkServer &server, Boss &boss,
                                    const Network::Message &requestMessage)
    {
        const Network::Message handled = server.handle(requestMessage);
        Network::Reply reply;
        std::string error;
        if (!Network::decodeReply(handled, reply, error) || reply.type != Network::ReplyType::Snapshot)
        {
            return handled;
        }

        Network::Request request;
        long long damage = 0;
        if (Network::decodeRequest(requestMessage, request, error) &&
            request.type == Network::RequestType::Position && parseDamageDealt(request.position.data, damage))
        {
            boss.reportDamage(request.playerId, damage);
        }

        reply.snapshot.platforms = boss.objects();
        return Network::encodeSnapshot(reply.snapshot);
    }

    // Lets the engine server handle a JOIN. If it welcomes the player, the reply
    // becomes a WELCOME pointing at `sessionEndpoint` with the boss in its
    // snapshot, and true comes back; otherwise `reply` is the server's own answer.
    bool welcomeWithBoss(Network::NetworkServer &server, Boss &boss, const Network::Message &requestMessage,
                         const std::string &sessionEndpoint, Network::Message &reply,
                         Network::PlayerId &playerId)
    {
        const Network::Message handled = server.handle(requestMessage);
        Network::Reply welcome;
        std::string error;
        if (!Network::decodeReply(handled, welcome, error) || welcome.type != Network::ReplyType::Welcome)
        {
            reply = handled;
            return false;
        }
        welcome.snapshot.platforms = boss.objects();
        playerId = welcome.playerId;
        reply = Network::encodeWelcome(welcome.playerId, welcome.snapshot, sessionEndpoint);
        return true;
    }

    // ---------------------------------------------------------------------------
    // Client workers
    // ---------------------------------------------------------------------------
    struct ClientWorker
    {
        std::shared_ptr<std::atomic<bool>> alive;
        std::thread thread;
    };

    // Every worker thread, and the session endpoint of every live session.
    struct ClientRegistry
    {
        std::mutex workersMutex;
        std::vector<ClientWorker> workers;
        std::mutex endpointsMutex;
        std::unordered_map<Network::SessionToken, std::string> endpointsByToken;

        void addWorker(ClientWorker worker)
        {
            const std::lock_guard<std::mutex> lock(workersMutex);
            workers.push_back(std::move(worker));
        }

        // Empty when this token has no live session.
        std::string endpointFor(const Network::SessionToken &token)
        {
            const std::lock_guard<std::mutex> lock(endpointsMutex);
            const auto existing = endpointsByToken.find(token);
            return existing != endpointsByToken.end() ? existing->second : std::string();
        }

        void setEndpoint(const Network::SessionToken &token, const std::string &endpoint)
        {
            const std::lock_guard<std::mutex> lock(endpointsMutex);
            endpointsByToken[token] = endpoint;
        }

        void forgetEndpoint(const Network::SessionToken &token)
        {
            const std::lock_guard<std::mutex> lock(endpointsMutex);
            endpointsByToken.erase(token);
        }

        void stopAll()
        {
            const std::lock_guard<std::mutex> lock(workersMutex);
            for (ClientWorker &worker : workers)
            {
                if (worker.alive)
                {
                    worker.alive->store(false);
                }
            }
        }

        void joinAll()
        {
            const std::lock_guard<std::mutex> lock(workersMutex);
            for (ClientWorker &worker : workers)
            {
                if (worker.thread.joinable())
                {
                    worker.thread.join();
                }
            }
            workers.clear();
        }
    };

    // Answers this client's requests until it leaves or the worker is cancelled.
    void serveClient(ReplySocket &socket, Network::NetworkServer &server, Boss &boss,
                     const std::atomic<bool> &alive)
    {
        while (alive.load() && g_running.load())
        {
            Network::Message requestMessage;
            if (!socket.receive(requestMessage))
            {
                continue;
            }

            Network::Request request;
            std::string error;
            const bool decoded = Network::decodeRequest(requestMessage, request, error);
            socket.send(handleWithBoss(server, boss, requestMessage));

            if (decoded && request.type == Network::RequestType::Leave)
            {
                break;
            }
        }
    }

    // One blocking REP loop per connected client. A slow client only stalls this
    // thread — other workers keep serving. Sockets stay on the worker thread
    // (ZeroMQ sockets are not thread-safe).
    void runClientWorker(Transport &transport, Network::NetworkServer &server, Boss &boss,
                         std::promise<std::string> endpointReady, std::shared_ptr<std::atomic<bool>> alive,
                         Network::SessionToken sessionToken, ClientRegistry &registry)
    {
        try
        {
            ReplySocket socket(transport, socketReceiveTimeout);
            // Bind on all interfaces; main rewrites the host for WELCOME via --advertise.
            endpointReady.set_value(socket.bind(workerBindEndpoint));
            serveClient(socket, server, boss, *alive);
        }
        catch (...)
        {
            try
            {
                endpointReady.set_value({});
            }
            catch (const std::future_error &)
            {
                // Endpoint was already published before the failure.
            }
        }

        registry.forgetEndpoint(sessionToken);
        alive->store(false);
    }

    // ---------------------------------------------------------------------------
    // The server
    // ---------------------------------------------------------------------------
    struct ServerOptions
    {
        std::string handshakeEndpoint = defaultServerBind;
        std::string advertiseHost = defaultAdvertiseHost;
    };

    bool parseArgs(int argc, char *argv[], ServerOptions &options)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--advertise")
            {
                if (i + 1 >= argc)
                {
                    std::cerr << "pokemon-hunt-server: --advertise requires a host\n";
                    return false;
                }
                options.advertiseHost = argv[++i];
                continue;
            }
            if (arg.rfind("--", 0) == 0)
            {
                std::cerr << "pokemon-hunt-server: unknown option " << arg << '\n';
                return false;
            }
            options.handshakeEndpoint = arg;
        }
        return true;
    }

    Network::ServerConfig makeServerConfig()
    {
        Network::ServerConfig config;
        config.spawnPoints = {{60.0F, 300.0F}, {300.0F, 300.0F}, {600.0F, 300.0F}, {860.0F, 300.0F}};
        return config;
    }

    class PokemonHuntServer
    {
    public:
        explicit PokemonHuntServer(ServerOptions options)
            : options_(std::move(options)),
              server_(clock_, makeServerConfig())
        {
        }

        ~PokemonHuntServer() { shutdown(); }

        PokemonHuntServer(const PokemonHuntServer &) = delete;
        PokemonHuntServer &operator=(const PokemonHuntServer &) = delete;

        // Serves JOINs until Ctrl-C. Returns the process exit code.
        int run()
        {
            int exitCode = 0;
            try
            {
                ReplySocket handshake(transport_, socketReceiveTimeout);
                handshake.bind(options_.handshakeEndpoint);
                startWorldThread();
                printBanner();
                acceptJoins(handshake);
            }
            catch (const std::exception &exception)
            {
                std::cerr << "Network server error: " << exception.what() << '\n';
                exitCode = 1;
            }
            shutdown();
            return exitCode;
        }

    private:
        // The boss runs on the server's real time, whatever speed any client
        // runs at, so it is in the same place on every screen.
        void startWorldThread()
        {
            worldThread_ = std::thread([this]
                                       {
            auto last = std::chrono::steady_clock::now();
            while (g_running.load()) {
                std::this_thread::sleep_for(worldTickInterval);
                const auto now = std::chrono::steady_clock::now();
                const float dt = std::min(std::chrono::duration<float>(now - last).count(), maxWorldStep);
                last = now;
                boss_.update(dt);
                server_.update();
            } });
        }

        void printBanner() const
        {
            std::cout << "Network server handshake listening on " << options_.handshakeEndpoint << '\n';
            std::cout << "Session endpoints advertise host " << options_.advertiseHost << '\n';
            std::cout << "Each JOIN spawns a dedicated per-client REP worker (no Router/Dealer).\n";
            std::cout << "A wild Mewtwo appeared! It moves on the server's real time.\n";
        }

        void acceptJoins(ReplySocket &handshake)
        {
            while (g_running.load())
            {
                Network::Message requestMessage;
                if (!handshake.receive(requestMessage))
                {
                    continue;
                }
                handshake.send(handleHandshake(requestMessage));
            }
        }

        Network::Message handleHandshake(const Network::Message &requestMessage)
        {
            Network::Request request;
            std::string error;
            if (!Network::decodeRequest(requestMessage, request, error))
            {
                return Network::encodeError(error);
            }
            if (request.type != Network::RequestType::Join)
            {
                return Network::encodeError("handshake accepts JOIN only");
            }

            // Rejoin: reuse the existing worker endpoint when this token is live.
            const std::string existing = registry_.endpointFor(request.sessionToken);
            if (!existing.empty())
            {
                Network::Message reply;
                Network::PlayerId playerId = 0;
                welcomeWithBoss(server_, boss_, requestMessage, existing, reply, playerId);
                return reply;
            }
            return joinNewClient(requestMessage, request.sessionToken);
        }

        // Starts a worker for a new session, then welcomes the player to it.
        Network::Message joinNewClient(const Network::Message &requestMessage, const Network::SessionToken &token)
        {
            auto alive = std::make_shared<std::atomic<bool>>(true);
            const std::string sessionEndpoint = startWorker(token, alive);
            if (sessionEndpoint.empty())
            {
                alive->store(false);
                return Network::encodeError("could not start client worker");
            }

            Network::Message reply;
            Network::PlayerId playerId = 0;
            if (!welcomeWithBoss(server_, boss_, requestMessage, sessionEndpoint, reply, playerId))
            {
                alive->store(false);
                return reply;
            }

            registry_.setEndpoint(token, sessionEndpoint);
            std::cout << "Client player " << playerId << " -> " << sessionEndpoint << '\n';
            return reply;
        }

        // Returns the endpoint to advertise for the new worker, or empty if it
        // could not bind.
        std::string startWorker(const Network::SessionToken &token, const std::shared_ptr<std::atomic<bool>> &alive)
        {
            std::promise<std::string> endpointReady;
            std::future<std::string> endpointFuture = endpointReady.get_future();

            ClientWorker worker;
            worker.alive = alive;
            worker.thread = std::thread(runClientWorker, std::ref(transport_), std::ref(server_), std::ref(boss_),
                                        std::move(endpointReady), alive, token, std::ref(registry_));
            registry_.addWorker(std::move(worker));

            const std::string boundEndpoint = endpointFuture.get();
            if (boundEndpoint.empty())
            {
                return {};
            }
            return Network::rewriteTcpEndpointHost(boundEndpoint, options_.advertiseHost);
        }

        // Stop background work while the server and transport are still alive (no detached UAF).
        void shutdown()
        {
            if (shutDown_)
            {
                return;
            }
            shutDown_ = true;
            g_running.store(false);
            registry_.stopAll();
            transport_.shutdown();

            if (worldThread_.joinable())
            {
                worldThread_.join();
            }
            registry_.joinAll();
        }

        ServerOptions options_;
        Transport transport_;
        RealTimeClock clock_;
        Network::NetworkServer server_;
        Boss boss_;
        ClientRegistry registry_;
        std::thread worldThread_;
        bool shutDown_ = false;
    };

} // namespace

int main(int argc, char *argv[])
{
    ServerOptions options;
    if (!parseArgs(argc, argv, options))
    {
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    PokemonHuntServer server(std::move(options));
    return server.run();
}
