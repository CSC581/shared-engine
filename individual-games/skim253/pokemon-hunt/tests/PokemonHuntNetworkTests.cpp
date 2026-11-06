// Loopback integration tests for clock sync, Psystrike, hit validation and knockout events.
#include "EventManager.hpp"
#include "Multiplayer.hpp"
#include "NetworkEventBridge.hpp"
#include "NetworkServerHost.hpp"
#include "PokemonHuntBoss.hpp"
#include "PokemonHuntPrediction.hpp"
#include "PokemonHuntEvents.hpp"
#include "PokemonHuntWorld.hpp"
#include "TimeSource.hpp"
#include "TimeUnits.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace PokemonHunt;

bool expect(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

Network::ServerConfig serverConfig()
{
    Network::ServerConfig config;
    config.spawnPoints = {{0.0F, 0.0F}};
    config.platforms = movingPlatformPaths();
    return config;
}

Network::HostConfig hostConfig()
{
    Network::HostConfig config;
    config.bindEndpoint = "tcp://127.0.0.1:*";
    config.advertiseHost = "127.0.0.1";
    config.pollInterval = std::chrono::milliseconds(10);
    return config;
}

// Running before any session opens.
struct StartedHost {
    StartedHost() { host.start(); }
    Network::NetworkServerHost host{serverConfig(), hostConfig()};
};

Multiplayer::Config link(const StartedHost& server, const std::string& name)
{
    Multiplayer::Config config;
    config.mode = Multiplayer::Mode::ClientServer;
    config.serverEndpoint = server.host.boundEndpoint();
    config.playerName = name;
    return config;
}

// Mewtwo's bot and one Pokemon on one loopback server.
class Raid {
public:
    Raid()
        : bossSession_(Multiplayer::Session::open(link(server_, bossPlayerName), clock_)),
          bossBridge_(boss_.events(), *bossSession_, sharedEventTypes()),
          pokemonEvents_(clock_),
          pokemon_(Multiplayer::Session::open(link(server_, "trainer"), clock_)),
          pokemonBridge_(pokemonEvents_, *pokemon_, sharedEventTypes())
    {
        // As in PokemonHuntServer.cpp: hits are checked against this session's poses.
        boss_.setPlayerLocator(sessionLocator(*bossSession_));
        pokemonEvents_.subscribe(Events::Death, [this](const Event& event) { deaths.push_back(event); });
        pokemonEvents_.subscribe(GameEvents::Psystrike, [this](const Event& event) {
            psystrikes.push_back(event);
            psystrikeArrivals.push_back(clock_.now());
        });
        pokemonEvents_.subscribe(GameEvents::ClockPong, [this](const Event& event) {
            if (event.getOr<std::int64_t>("player", 0) == static_cast<std::int64_t>(pokemon_->localPlayerId())) {
                bossClock.receive(event.get<std::int64_t>("sentAt"), clock_.now(), event.get<std::int64_t>("bossTime"));
            }
        });
        lastStep_ = clock_.now();
    }

    // One frame for both. Mewtwo steps on real time, as on the server. The
    // Pokemon pings Mewtwo's clock and stands on its head unless farAway.
    void step()
    {
        const std::int64_t now = clock_.now();
        boss_.update(static_cast<float>(now - lastStep_) / static_cast<float>(kNsPerSec));
        lastStep_ = now;
        bossSession_->update();
        bossBridge_.apply();
        bossSession_->publishLocalPlayer(boss_.x(), boss_.y(), encodeBossWorld(boss_.objects()));

        pokemon_->update();
        pokemonBridge_.apply();
        if (now >= nextPing_) {
            pokemonEvents_.raise(Event{GameEvents::ClockPing}.set("sentAt", now));
            nextPing_ = now + kNsPerSec / 4;
        }
        pokemonEvents_.dispatch();
        const float x = farAway ? boss_.x() - 900.0F : boss_.x() + bossWidth * 0.25F;
        const float y = farAway ? groundY - pokemonSize : boss_.y() - pokemonSize;
        pokemon_->publishLocalPlayer(x, y, "0 0 1 0 0 0 0");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    bool waitFor(const std::function<bool()>& done, int seconds = 3)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < deadline) {
            step();
            if (done()) {
                return true;
            }
        }
        return false;
    }

    void idle(int seconds)
    {
        waitFor([] { return false; }, seconds);
    }

    void idleFor(std::int64_t ns)
    {
        const std::int64_t until = clock_.now() + ns;
        while (clock_.now() < until) {
            step();
        }
    }

    void stomp() { pokemonEvents_.raise(Event{GameEvents::Hit}.set("kind", GameEvents::HitStomp).set("points", 0)); }

    bool joined() const { return bossSession_->remotePlayers().size() == 1 && pokemon_->remotePlayers().size() == 1; }
    Boss& boss() { return boss_; }
    std::uint32_t bossId() const { return bossSession_->localPlayerId(); }
    std::uint32_t pokemonId() const { return pokemon_->localPlayerId(); }
    std::int64_t now() const { return clock_.now(); }

    bool farAway = false;
    BossClockSync bossClock;
    std::vector<Event> deaths;
    std::vector<Event> psystrikes;
    std::vector<std::int64_t> psystrikeArrivals;

private:
    // In construction order; bridges are destroyed before their sessions and managers.
    StartedHost server_;
    RealTimeClock clock_;
    Boss boss_{{}, 7U};
    std::unique_ptr<Multiplayer::Session> bossSession_;
    NetworkEventBridge bossBridge_;
    EventManager pokemonEvents_;
    std::unique_ptr<Multiplayer::Session> pokemon_;
    NetworkEventBridge pokemonBridge_;
    std::int64_t lastStep_ = 0;
    std::int64_t nextPing_ = 0;
};

bool clockSyncAndPsystrike(Raid& raid)
{
    bool passed = expect(raid.waitFor([&] { return raid.bossClock.synced(); }),
                         "a ClockPing comes back as this Pokemon's pong");
    const std::int64_t error = raid.bossClock.toLocal(raid.boss().clockUs()) - raid.now();
    passed &= expect(std::llabs(error) < 30 * kNsPerMs, "the synced clock places Mewtwo's time on ours within 30 ms (was " +
                                                           std::to_string(error / kNsPerMs) + " ms)");

    raid.boss().psystrike();
    passed &= expect(raid.waitFor([&] { return raid.psystrikes.size() == 2; }),
                     "Mewtwo's Psystrike reaches the Pokemon as a charge, then a strike");
    if (raid.psystrikes.size() != 2) {
        return false;
    }
    const Event& charge = raid.psystrikes[0];
    const Event& strike = raid.psystrikes[1];
    passed &= expect(charge.get<std::string>("stage") == "charge" && strike.get<std::string>("stage") == "strike" &&
                         strike.origin == raid.bossId(),
                     "the stages arrive in order, from Mewtwo's player");
    const double apart = static_cast<double>(raid.psystrikeArrivals[1] - raid.psystrikeArrivals[0]) / kNsPerSec;
    passed &= expect(apart > psystrikeChargeSeconds * 0.8 && apart < psystrikeChargeSeconds * 1.5,
                     "the strike arrives one charge time after the warning");
    const double late =
        static_cast<double>(raid.psystrikeArrivals[1] - raid.bossClock.toLocal(strike.get<std::int64_t>("at"))) /
        kNsPerSec;
    passed &= expect(late > -0.03 && late < 0.2,
                     "the strike's `at` says how late it arrived (" + std::to_string(late) + " s)");
    return passed;
}

bool hitsAreCheckedAndTheKnockoutComesBack(Raid& raid)
{
    Boss& boss = raid.boss();
    raid.farAway = true;
    raid.idle(1);
    raid.stomp();
    raid.idle(1);
    bool passed = expect(boss.hp() == bossMaxHp && boss.refusedHits() == 1,
                         "a stomp from a Pokemon nowhere near Mewtwo's head is refused");
    raid.farAway = false;
    raid.idle(1);

    raid.stomp();
    passed &= expect(raid.waitFor([&] { return boss.hp() == bossMaxHp - stompDamage; }),
                     "a stomp from on top of Mewtwo crosses the server and takes HP exactly once");

    // One stomp at a time, at a stomper's pace (a bounce takes longer than
    // this), stays within Mewtwo's budget of hitBudgetPerSecond.
    passed &= expect(raid.waitFor(
                         [&] {
                             if (raid.deaths.empty() && !boss.fainted()) {
                                 const int before = boss.hp();
                                 raid.stomp();
                                 raid.waitFor([&] { return boss.hp() < before || boss.fainted(); });
                                 raid.idleFor(300 * kNsPerMs);
                             }
                             return !raid.deaths.empty();
                         },
                         20),
                     "Mewtwo's knockout DeathEvent reaches the Pokemon");
    if (!raid.deaths.empty()) {
        const Event& death = raid.deaths.front();
        passed &= expect(boss.fainted(), "the hits knocked Mewtwo out");
        passed &= expect(death.origin == raid.bossId(), "the death comes from Mewtwo's player");
        passed &= expect(death.getOr<std::int64_t>("finisher", 0) == static_cast<std::int64_t>(raid.pokemonId()),
                         "it names this Pokemon as the finisher");
        passed &= expect(!death.has("object"), "Mewtwo's local object id does not cross the network");
    }
    for (int i = 0; i < 30; ++i) {
        raid.step();
    }
    passed &= expect(raid.deaths.size() == 1, "the knockout arrives once");
    passed &= expect(boss.refusedHits() == 1, "no paced stomp from on top of Mewtwo was refused");
    return passed;
}

} // namespace

int main()
{
    Raid raid;
    bool passed = expect(raid.waitFor([&] { return raid.joined(); }),
                         "Mewtwo and the Pokemon join one server and see each other");
    passed = passed && clockSyncAndPsystrike(raid);
    passed = passed && hitsAreCheckedAndTheKnockoutComesBack(raid);
    if (passed) {
        std::cout << "pokemon-hunt-network-tests: all checks passed\n";
    }
    return passed ? 0 : 1;
}
