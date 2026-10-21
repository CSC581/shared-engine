#pragma once

#include "Collision.hpp"
#include "Color.hpp"
#include "Components.hpp"
#include "Multiplayer.hpp"
#include "World.hpp"

#include <cstdint>
#include <vector>

// The station as GameObjects: one factory per kind of object, plus the
// astronaut's platformer rules as a component. SDL-free, so the level can be
// built and simulated in tests without a window.
namespace StarfallSalvage {

// Collider::layers bits: what may touch what.
constexpr std::uint32_t solidLayer = 1U << 0U;
constexpr std::uint32_t hazardLayer = 1U << 1U;
constexpr std::uint32_t zoneLayer = 1U << 2U;

// Renderable::layer values: draw order.
constexpr int deckDrawLayer = 1;
constexpr int fixtureDrawLayer = 2;

// Each kind of deck has its own colours, so the static platforms differ in
// colour as well as position and size.
enum class DeckKind { Hull, West, East };

struct DeckPalette {
    Color base;
    Color top;
    Color line;
    Color bolt;
};

DeckPalette paletteFor(DeckKind kind);
DeckKind deckKindFor(std::size_t deckIndex);

// Data only: which palette a deck is drawn with.
class DeckStyle : public Component {
public:
    explicit DeckStyle(DeckKind kind) : kind(kind) {}

    DeckKind kind;
};

// The local astronaut's platformer rules. Input becomes velocity, with a
// double jump and a capped fall speed. Its Collider callbacks then work out
// what happened in the World's collision pass: landing on a deck, shuttle or
// the bulkhead, touching a drone, or falling into a death zone.
//
// update() runs on a FrameWorker through updateComponents(time, "player");
// everything else is called on the main thread, outside that worker.
class Astronaut : public Component {
public:
    struct Input {
        float axis = 0.0F;
        bool jump = false;
    };

    void onAttach() override;
    void update(const FrameTime& time) override;
    int priority() const override { return UpdateOrder::Behavior; }

    // Before World::resolveCollisions(): whether the astronaut is standing on
    // anything is recomputed from scratch by that pass.
    void beginCollisionPass();

    // After NetworkWorldSync::apply(): moves the astronaut by however far the
    // shuttle it stands on moved since the last frame. The shuttle may be
    // null when it has disappeared from the session.
    void rideShuttle(const GameObject* shuttle);
    void detachFromShuttle();

    // Back to `spawn`, at rest.
    void respawn();

    Input input;
    Rect spawn{};
    int lives = 3;
    float invulnerableSeconds = 0.0F;
    bool grounded = false;
    int jumpsUsed = 0;
    // Network id of the shuttle underfoot, or 0.
    std::uint32_t supportId = 0;

private:
    void touch(GameObject& other);
    void hitDrone();

    Transform* transform_ = nullptr;
    Motion* motion_ = nullptr;
    float supportX_ = 0.0F;
    float supportY_ = 0.0F;
};

// Fixed parts of the station the game updates or reads every frame.
struct Station {
    std::vector<GameObject*> spawns;
    std::vector<GameObject*> cargo;
    std::vector<GameObject*> relayPads;
    std::vector<GameObject*> gatePlates;
    GameObject* bulkhead = nullptr;
};

GameObject& makeDeck(World& world, const Rect& box, DeckKind kind);
GameObject& makeSpawnPoint(World& world, float x, float y);
GameObject& makeDeathZone(World& world, const Rect& box);
GameObject& makeFixture(World& world, const char* tag, const Rect& box, Color color);
GameObject& makeBulkhead(World& world);
GameObject& makePlayer(World& world);

// NetworkWorldSync factories for remote astronauts and server platforms.
GameObject& makeRemotePlayer(World& world, const Multiplayer::Player& player);
GameObject& makeNetworkPlatform(World& world, const Multiplayer::Platform& platform);

// Every deck, spawn point, death zone, crate, pad, plate and the bulkhead.
Station buildStation(World& world);

} // namespace StarfallSalvage
