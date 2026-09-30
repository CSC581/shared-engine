// Headless server for Pokemon Hunt. It runs the engine's NetworkServerHost as
// is, plus Mewtwo as a bot player that joins both the client-server session
// and the peer-to-peer mesh. The bot publishes the boss in its player data and
// reads each Pokemon's damage from theirs.
//
//   ./build/pokemon-hunt-server            # tcp://*:5600, boss peer on 7290
//   ./build/pokemon-hunt-server --rates    # log each player's send rate
#include "Collision.hpp"
#include "Endpoint.hpp"
#include "Entity.hpp"
#include "Multiplayer.hpp"
#include "NetworkServerHost.hpp"
#include "Physics.hpp"
#include "PokemonHuntWorld.hpp"
#include "TimeSource.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
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
    constexpr auto trafficLogInterval = std::chrono::milliseconds(1000);

    constexpr const char *defaultAdvertiseHost = "127.0.0.1";
    // A restarted server greets peer 1, whose roster reintroduces Mewtwo to
    // the rest of a mesh that is already running.
    constexpr const char *defaultFirstPeerEndpoint = "tcp://127.0.0.1:7300";

    std::atomic<bool> g_running{true};

    void onSignal(int)
    {
        g_running.store(false);
    }

    // The host logs from its own threads, and the bot from the main one.
    std::mutex g_logMutex;

    void say(const std::string &line)
    {
        const std::lock_guard<std::mutex> lock(g_logMutex);
        std::cout << line << std::endl;
    }

    // ---------------------------------------------------------------------------
    // The boss
    // ---------------------------------------------------------------------------
    // Mewtwo, Bowser style: walks back and forth across the floor, and every few
    // seconds either jumps or stops to charge up and fire a shadow ball. It owns
    // its HP: when that runs out it vanishes, and drops back in at a random spot
    // bossRespawnSeconds later.
    // Used only by the bot's loop, on the main thread.
    class Boss
    {
    public:
        // Physics gravity is one global value; in this process only the boss falls.
        Boss() { Physics::setGravity(bossGravity); }

        void update(float dt)
        {
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
        // only what is new since its last report comes off the HP. `player` must
        // be unique across every session the bot is in. Damage that lands while
        // the boss is fainted is spent on nothing.
        void reportDamage(std::uint64_t player, long long damage)
        {
            long long &last = reported_[player];
            // A total that went down is a new Pokemon that took over a departed
            // one's id; it starts counting from here.
            if (damage < last)
            {
                last = damage;
                return;
            }
            const long long fresh = damage - last;
            if (fresh == 0)
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

        std::vector<Multiplayer::Platform> objects() const
        {
            std::vector<Multiplayer::Platform> result;
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

        float x() const { return body_.getX(); }
        float y() const { return body_.getY(); }

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

        static Multiplayer::Platform toPlatform(std::uint32_t id, const Entity &entity)
        {
            const Rect bounds = entity.getBounds();
            return {id, bounds.x, bounds.y, bounds.width, bounds.height};
        }

        float direction() const { return facingLeft_ ? -1.0F : 1.0F; }

        // --- update() steps. -----------------------------------------------------
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
            say("Mewtwo fainted (KO " + std::to_string(knockouts_) + "); back in " +
                std::to_string(static_cast<int>(bossRespawnSeconds)) + "s");
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
            say("Mewtwo is back at x=" + std::to_string(static_cast<int>(body_.getX())));
        }

        // --- objects() pieces. ---------------------------------------------------
        // See bossStatusId for how the fields are packed.
        Multiplayer::Platform statusObject() const
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
        std::unordered_map<std::uint64_t, long long> reported_;
    };

    // ---------------------------------------------------------------------------
    // The bot
    // ---------------------------------------------------------------------------
    // Mewtwo as a player: one engine session per architecture, each carrying
    // the same boss. Every tick it moves the boss on real time, reads the
    // damage the Pokemon report in their own data, and publishes the boss world
    // as its data.
    class BossBot
    {
    public:
        explicit BossBot(const TimeSource &clock) : clock_(clock) {}

        // A link that fails to open (a taken port, say) is reported and left
        // out; the other mode still works.
        void join(const std::string &label, const Multiplayer::Config &config)
        {
            auto session = Multiplayer::Session::open(config, clock_);
            if (session->state() == Multiplayer::State::Failed)
            {
                say("Mewtwo could not join " + label + ": " + session->status());
                return;
            }
            links_.push_back({label, std::move(session), false});
        }

        void run()
        {
            auto last = std::chrono::steady_clock::now();
            while (g_running.load())
            {
                std::this_thread::sleep_for(worldTickInterval);
                const auto now = std::chrono::steady_clock::now();
                const float dt = std::min(std::chrono::duration<float>(now - last).count(), maxWorldStep);
                last = now;

                boss_.update(dt);
                for (std::size_t i = 0; i < links_.size(); ++i)
                {
                    links_[i].session->update();
                    reportReadiness(links_[i]);
                    readDamage(i, *links_[i].session);
                }

                const std::string data = encodeBossWorld(boss_.objects());
                for (Link &link : links_)
                {
                    link.session->publishLocalPlayer(boss_.x(), boss_.y(), data);
                }
            }
        }

    private:
        struct Link
        {
            std::string label;
            std::unique_ptr<Multiplayer::Session> session;
            bool announced;
        };

        static void reportReadiness(Link &link)
        {
            if (!link.announced && link.session->state() == Multiplayer::State::Ready)
            {
                link.announced = true;
                say("Mewtwo joined the " + link.label + " session");
            }
        }

        // Player ids are only unique within one session, so the link index
        // goes into the key too.
        void readDamage(std::size_t linkIndex, const Multiplayer::Session &session)
        {
            for (const Multiplayer::Player &player : session.remotePlayers())
            {
                long long damage = 0;
                if (parseDamageDealt(player.data, damage))
                {
                    boss_.reportDamage((static_cast<std::uint64_t>(linkIndex) << 32) | player.id, damage);
                }
            }
        }

        const TimeSource &clock_;
        Boss boss_;
        std::vector<Link> links_;
    };

    // ---------------------------------------------------------------------------
    // Setup
    // ---------------------------------------------------------------------------
    struct ServerOptions
    {
        std::string handshakeEndpoint = defaultServerBind;
        std::string advertiseHost = defaultAdvertiseHost;
        std::string firstPeer = defaultFirstPeerEndpoint;
        bool logRates = false;
    };

    bool parseArgs(int argc, char *argv[], ServerOptions &options)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--rates")
            {
                options.logRates = true;
                continue;
            }
            if (arg == "--advertise" || arg == "--peer")
            {
                if (i + 1 >= argc)
                {
                    std::cerr << "pokemon-hunt-server: " << arg << " requires a value\n";
                    return false;
                }
                (arg == "--advertise" ? options.advertiseHost : options.firstPeer) = argv[++i];
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

    Network::HostConfig makeHostConfig(const ServerOptions &options)
    {
        Network::HostConfig config;
        config.mode = Network::HostMode::Dedicated;
        config.bindEndpoint = options.handshakeEndpoint;
        config.advertiseHost = options.advertiseHost;
        config.log = say;
        if (options.logRates)
        {
            config.trafficLogInterval = trafficLogInterval;
        }
        return config;
    }

    // Mewtwo joins its own host over loopback, like any client would.
    Multiplayer::Config clientServerLink(const Network::NetworkServerHost &host)
    {
        Multiplayer::Config config;
        config.mode = Multiplayer::Mode::ClientServer;
        config.serverEndpoint = Net::rewriteTcpEndpointHost(host.boundEndpoint(), "127.0.0.1");
        config.playerName = bossPlayerName;
        return config;
    }

    // Mewtwo is a peer with no world authority of its own to poll: it is the
    // authority, and its state goes out on its own PUB like any peer's.
    Multiplayer::Config peerLink(const ServerOptions &options)
    {
        Multiplayer::Config config;
        config.mode = Multiplayer::Mode::PeerToPeer;
        config.serverEndpoint.clear();
        config.playerName = bossPlayerName;
        config.peerId = bossPeerId;
        config.basePort = bossPeerBasePort;
        config.advertiseHost = options.advertiseHost;
        config.bootstrapPeers = {options.firstPeer};
        return config;
    }

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

    try
    {
        // Declared in this order so the bot's sessions leave before the host
        // they are connected to stops.
        Network::NetworkServerHost host(makeServerConfig(), makeHostConfig(options));
        host.start();
        RealTimeClock clock;
        BossBot bot(clock);
        bot.join("client-server", clientServerLink(host));
        bot.join("peer-to-peer", peerLink(options));

        say("Mewtwo's peer listens on port " + std::to_string(bossPeerBasePort));
        say("A wild Mewtwo appeared! It moves on the server's real time.");
        bot.run();
    }
    catch (const std::exception &exception)
    {
        std::cerr << "pokemon-hunt-server: " << exception.what() << '\n';
        return 1;
    }
    return 0;
}
