#pragma once

#include "Collision.hpp"
#include "Color.hpp"
#include "Components.hpp"
#include "Multiplayer.hpp"
#include "StarfallSalvageRules.hpp"
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
constexpr std::uint32_t pickupLayer = 1U << 3U;

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
// double jump and a capped fall speed, in whichever direction gravity pulls.
// Its Collider callback works out whether the collision pass left it standing
// on a deck, shuttle or the bulkhead: on top normally, underneath when its
// gravity is flipped. Gameplay (drones, death zones, crates, gravity panels)
// arrives as CollisionEvents instead; see StationEvents.
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
    // Stops dead where it is: no velocity, not standing on anything.
    void halt();

    // Back to `spawn`, or to (x, y), at rest.
    void respawn();
    void respawnAt(float x, float y);

    // Gravity pulls up while flipped: the astronaut falls toward the ceiling,
    // stands on undersides and jumps downward.
    void setFlipped(bool flipped);
    bool flipped() const { return flipped_; }

    Input input;
    Rect spawn{};
    int lives = startingLives;
    float invulnerableSeconds = 0.0F;
    bool grounded = false;
    int jumpsUsed = 0;
    // Network id of the shuttle underfoot, or 0.
    std::uint32_t supportId = 0;

private:
    void touch(GameObject& other);

    Transform* transform_ = nullptr;
    Motion* motion_ = nullptr;
    Gravity* gravity_ = nullptr;
    bool flipped_ = false;
    float supportX_ = 0.0F;
    float supportY_ = 0.0F;
};

// Fixed parts of the station the game updates or reads every frame.
struct Station {
    std::vector<GameObject*> spawns;
    std::vector<GameObject*> cargo;
    std::vector<GameObject*> relayPads;
    std::vector<GameObject*> gatePlates;
    std::vector<GameObject*> gravityPanels;
    GameObject* bulkhead = nullptr;
};

GameObject& makeDeck(World& world, const Rect& box, DeckKind kind);
GameObject& makeSpawnPoint(World& world, float x, float y);
GameObject& makeDeathZone(World& world, const Rect& box);
GameObject& makeFixture(World& world, const char* tag, const Rect& box, Color color);
// A crate: a fixture with a trigger collider, so touching it raises a
// CollisionEvent.
GameObject& makeCargo(World& world, const Rect& box);
GameObject& makeGravityPanel(World& world, const Rect& box);
GameObject& makeBulkhead(World& world);
GameObject& makePlayer(World& world);

// NetworkWorldSync factories for remote astronauts and server platforms.
GameObject& makeRemotePlayer(World& world, const Multiplayer::Player& player);
GameObject& makeNetworkPlatform(World& world, const Multiplayer::Platform& platform);

// Every deck (and the ceiling), spawn point, death zone, crate, gravity panel,
// pad, plate and the bulkhead.
Station buildStation(World& world);

} // namespace StarfallSalvage
