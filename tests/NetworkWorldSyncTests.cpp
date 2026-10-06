#include "Components.hpp"
#include "Multiplayer.hpp"
#include "NetworkWorldSync.hpp"
#include "World.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "Test failed: " << message << '\n';
    }
    return condition;
}

// A session whose contents the test sets directly, standing in for whatever
// a real one last received over the network.
class FakeSession final : public Multiplayer::Session {
public:
    std::vector<Multiplayer::Player> players;
    std::vector<Multiplayer::Platform> platformList;

    void update() override {}
    void publishLocalPlayer(float, float, const std::string&) override {}
    void leave() override {}
    Multiplayer::PlayerId localPlayerId() const override { return 1; }
    const std::vector<Multiplayer::Player>& remotePlayers() const override { return players; }
    const std::vector<Multiplayer::Platform>& platforms() const override { return platformList; }
    Multiplayer::State state() const override { return Multiplayer::State::Ready; }
    Multiplayer::AuthorityState authorityState() const override { return Multiplayer::AuthorityState::Ready; }
    std::string status() const override { return "fake"; }
    Multiplayer::Mode mode() const override { return Multiplayer::Mode::ClientServer; }
};

struct Counts {
    int players = 0;
    int platforms = 0;
};

NetworkWorldSync makeSync(World& world, Counts& counts)
{
    return NetworkWorldSync(
        world,
        [&counts](World& w, const Multiplayer::Player&) -> GameObject& {
            ++counts.players;
            GameObject& object = w.create("remote-player");
            object.add<Transform>(0.0F, 0.0F, 20.0F, 20.0F);
            return object;
        },
        [&counts](World& w, const Multiplayer::Platform&) -> GameObject& {
            ++counts.platforms;
            return w.create("remote-platform");
        });
}

bool testPlayersFollowTheSession()
{
    bool passed = true;
    World world;
    Counts counts;
    NetworkWorldSync sync = makeSync(world, counts);
    FakeSession session;

    // Appears.
    session.players = {{2, "B", 10.0F, 20.0F, {}}};
    sync.apply(session);
    GameObject* b = sync.remotePlayer(2);
    passed &= expect(b != nullptr && b->isActive() && counts.players == 1,
                     "a newly seen player should get one active object");
    passed &= expect(b != nullptr && b->get<NetworkIdentity>() != nullptr &&
                         b->get<NetworkIdentity>()->kind == NetworkIdentity::Kind::Player &&
                         b->get<NetworkIdentity>()->id == 2,
                     "the object should carry the player's network identity");
    passed &= expect(b != nullptr && b->get<Transform>()->x == 10.0F && b->get<Transform>()->y == 20.0F &&
                         b->get<Transform>()->width == 20.0F,
                     "the object should be at the reported pose, keeping the factory's size");

    // Moves; applying again builds nothing new.
    session.players[0].x = 15.0F;
    sync.apply(session);
    sync.apply(session);
    passed &= expect(sync.remotePlayer(2) == b && b->get<Transform>()->x == 15.0F, "the object should follow");
    passed &= expect(counts.players == 1 && world.objects().size() == 1, "re-applying should not duplicate");

    // A second player joins, then the first disconnects.
    session.players.push_back({3, "C", 0.0F, 0.0F, {}});
    sync.apply(session);
    session.players.erase(session.players.begin());
    sync.apply(session);
    world.flushDestroyed();
    passed &= expect(sync.remotePlayer(2) == nullptr && sync.remotePlayer(3) != nullptr &&
                         world.objects().size() == 1,
                     "a player the session no longer reports should leave the scene");

    return passed;
}

bool testPlatformsFollowTheSession()
{
    bool passed = true;
    World world;
    Counts counts;
    NetworkWorldSync sync = makeSync(world, counts);
    FakeSession session;

    session.platformList = {{7, 100.0F, 200.0F, 96.0F, 20.0F}};
    sync.apply(session);
    GameObject* platform = sync.platform(7);
    passed &= expect(platform != nullptr && counts.platforms == 1 &&
                         platform->get<NetworkIdentity>()->kind == NetworkIdentity::Kind::Platform,
                     "a platform should get one object with a platform identity");

    session.platformList[0].x = 110.0F;
    sync.apply(session);
    const Transform& transform = *platform->get<Transform>();
    passed &= expect(transform.x == 110.0F && transform.y == 200.0F && transform.width == 96.0F &&
                         transform.height == 20.0F && counts.platforms == 1,
                     "a platform should take the reported pose and size");

    return passed;
}

// The game deleting a synced object must not leave anything dangling: it is
// rebuilt while the session still reports it.
bool testObjectDestroyedByTheGame()
{
    bool passed = true;
    World world;
    Counts counts;
    NetworkWorldSync sync = makeSync(world, counts);
    FakeSession session;

    session.players = {{4, "D", 1.0F, 2.0F, {}}};
    sync.apply(session);
    world.destroy(sync.remotePlayer(4)->id());
    world.flushDestroyed();
    passed &= expect(sync.remotePlayer(4) == nullptr, "a destroyed object should not be returned");

    sync.apply(session);
    passed &= expect(sync.remotePlayer(4) != nullptr && counts.players == 2 && world.objects().size() == 1,
                     "the object should be rebuilt while the session still reports it");

    return passed;
}

bool testFactoriesRequired()
{
    World world;
    bool threw = false;
    try {
        NetworkWorldSync sync(world, nullptr, nullptr);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    return expect(threw, "missing factories should be refused");
}

} // namespace

int main()
{
    bool passed = true;
    passed &= testPlayersFollowTheSession();
    passed &= testPlatformsFollowTheSession();
    passed &= testObjectDestroyedByTheGame();
    passed &= testFactoriesRequired();

    if (passed) {
        std::cout << "NetworkWorldSync tests passed.\n";
        return 0;
    }
    return 1;
}
