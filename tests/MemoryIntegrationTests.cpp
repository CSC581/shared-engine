#include "Components.hpp"
#include "Engine.hpp"
#include "EventManager.hpp"
#include "Multiplayer.hpp"
#include "NetworkWorldSync.hpp"
#include "ReplaySystem.hpp"
#include "TimeSource.hpp"
#include "Timeline.hpp"
#include "World.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* what, int line)
{
    if (!ok) {
        std::cerr << "line " << line << ": " << what << '\n';
        ++failures;
    }
}
#define CHECK(x) check((x), #x, __LINE__)

GameObject& square(World& world, const char* tag, Color color, int layer = 0)
{
    auto& object = world.create(tag);
    object.add<Transform>(0.0F, 0.0F, 8.0F, 8.0F);
    object.add<Renderable>(color).layer = layer;
    object.setActive(true);
    return object;
}

Color centerPixel(const World& world)
{
    SDL_Surface* surface = SDL_CreateSurface(8, 8, SDL_PIXELFORMAT_RGBA32);
    if (surface == nullptr) throw std::runtime_error(SDL_GetError());
    SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(surface);
    if (renderer == nullptr) {
        SDL_DestroySurface(surface);
        throw std::runtime_error(SDL_GetError());
    }
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    renderWorld(renderer, world);
    SDL_RenderPresent(renderer);
    Color result{};
    const bool read = SDL_ReadSurfacePixel(surface, 4, 4, &result.red, &result.green,
                                          &result.blue, &result.alpha);
    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(surface);
    if (!read) throw std::runtime_error(SDL_GetError());
    return result;
}

void renderingKeepsCreationOrderAfterReuse()
{
    World world(2);
    auto& old = square(world, "old", Color{255, 0, 0});
    square(world, "survivor", Color{0, 0, 255});
    CHECK(centerPixel(world).blue == 255);
    const auto address = reinterpret_cast<std::uintptr_t>(&old);
    world.destroy(old.id());
    world.flushDestroyed();
    auto& replacement = square(world, "replacement", Color{255, 0, 0});
    CHECK(reinterpret_cast<std::uintptr_t>(&replacement) == address);
    CHECK(centerPixel(world).red == 255);
}

void renderingKeepsLayersAndVisibilityAfterReuse()
{
    World world(2);
    auto& old = square(world, "old", Color{255, 0, 0}, 4);
    square(world, "survivor", Color{0, 0, 255}, 2);
    CHECK(centerPixel(world).red == 255);
    world.destroy(old.id());
    world.flushDestroyed();
    auto& replacement = square(world, "replacement", Color{255, 0, 0});
    CHECK(centerPixel(world).blue == 255);
    replacement.get<Renderable>()->layer = 5;
    CHECK(centerPixel(world).red == 255);
    replacement.get<Renderable>()->visible = false;
    CHECK(centerPixel(world).blue == 255);
}

void replaySeparatesRecycledLifetimes()
{
    ManualClock real;
    Timeline game(real, 1000);
    EventManager events(game);
    World world(1);
    ReplaySystem replay(events, game, real, 1000);
    const auto control = [&](const char* type) {
        events.raise(Event{type});
        events.dispatch();
    };
    auto& old = square(world, "old", Color{255, 0, 0});
    old.get<Transform>()->x = 1.0F;
    const auto oldId = old.id();
    const auto address = reinterpret_cast<std::uintptr_t>(&old);
    control(Events::ReplayRecordStart);
    replay.capture(world);
    world.destroy(oldId);
    world.flushDestroyed();
    auto& replacement = square(world, "new", Color{0, 0, 255});
    replacement.get<Transform>()->x = 90.0F;
    CHECK(reinterpret_cast<std::uintptr_t>(&replacement) == address);
    real.advance(100'000);
    replay.capture(world);
    control(Events::ReplayRecordStop);
    CHECK(replay.frameCount() == 2);
    control(Events::ReplayPlayStart);
    CHECK(replay.isPlaying());
    std::vector<const GameObject*> active;
    for (const auto& object : replay.world().objects()) {
        if (object->isActive()) active.push_back(object.get());
    }
    CHECK(active.size() == 1);
    if (active.size() == 1) {
        CHECK(active[0]->tag() == "old");
        CHECK(active[0]->get<Transform>()->x == 1.0F);
        CHECK(active[0]->get<Renderable>()->color.red == 255);
    }
    real.advance(100'000);
    replay.update();
    active.clear();
    for (const auto& object : replay.world().objects()) {
        if (object->isActive()) active.push_back(object.get());
    }
    CHECK(active.size() == 1);
    if (active.size() == 1) {
        CHECK(active[0]->tag() == "new");
        CHECK(active[0]->get<Transform>()->x == 90.0F);
        CHECK(active[0]->get<Renderable>()->color.blue == 255);
    }
    control(Events::ReplayPlayStop);
    CHECK(!game.isPaused());
    CHECK(world.find(oldId) == nullptr);
    CHECK(world.find(replacement.id()) == &replacement);
    CHECK(world.tryCreate() == nullptr);
}

// Mirrors the existing NetworkWorldSyncTests fake: no sockets or network thread.
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
    void publishEvent(const Event&) override {}
    std::vector<Event> takeRemoteEvents() override { return {}; }
};

void networkMappingsFollowIdsAfterReuse()
{
    World world(1);
    FakeSession session;
    NetworkWorldSync sync(world,
        [](World& world, const Multiplayer::Player&) -> GameObject& {
            auto& object = world.create("remote");
            object.add<Transform>(0.0F, 0.0F, 8.0F, 8.0F);
            return object;
        },
        [](World& world, const Multiplayer::Platform&) -> GameObject& { return world.create("platform"); });
    session.players = {{2, "remote", 10.0F, 20.0F, {}}};
    sync.apply(session);
    auto* old = sync.remotePlayer(2);
    CHECK(old != nullptr);
    if (old == nullptr) return;
    const auto oldId = old->id();
    const auto address = reinterpret_cast<std::uintptr_t>(old);
    world.destroy(oldId);
    world.flushDestroyed();
    session.players[0].x = 75.0F;
    sync.apply(session);
    auto* replacement = sync.remotePlayer(2);
    CHECK(replacement != nullptr);
    if (replacement == nullptr) return;
    CHECK(reinterpret_cast<std::uintptr_t>(replacement) == address);
    CHECK(replacement->id() > oldId);
    CHECK(replacement->get<Transform>()->x == 75.0F);
    CHECK(replacement->get<NetworkIdentity>()->id == 2);
    world.destroy(oldId);
    world.flushDestroyed();
    CHECK(sync.remotePlayer(2) == replacement);
    session.players.clear();
    sync.apply(session);
    world.flushDestroyed();
    CHECK(sync.remotePlayer(2) == nullptr);
    CHECK(world.objects().empty());
    CHECK(world.tryCreate() != nullptr);
}
} // namespace

int main()
{
    const std::pair<const char*, void (*)()> cases[] = {
        {"render creation order after reuse", renderingKeepsCreationOrderAfterReuse},
        {"render layers and visibility after reuse", renderingKeepsLayersAndVisibilityAfterReuse},
        {"replay separates recycled lifetimes", replaySeparatesRecycledLifetimes},
        {"network mappings follow fresh IDs", networkMappingsFollowIdsAfterReuse},
    };
    for (const auto& [name, run] : cases) {
        const int before = failures;
        try {
            run();
        } catch (const std::exception& error) {
            std::cerr << name << ": " << error.what() << '\n';
            ++failures;
        }
        std::cout << (before == failures ? "PASS " : "FAIL ") << name << '\n';
    }
    return failures == 0 ? 0 : 1;
}
