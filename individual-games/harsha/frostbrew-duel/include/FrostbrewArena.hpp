#pragma once

#include "FrostbrewRules.hpp"
#include "FrostbrewTerrain.hpp"
#include "Color.hpp"
#include "EventManager.hpp"
#include "World.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace Frostbrew {
struct Point { float x; float y; };

class Arena {
public:
    static constexpr int Width = 1200;
    static constexpr int Height = 800;
    static constexpr double Step = 1.0 / 240.0;

    explicit Arena(EventManager& events, std::size_t capacity = 128, std::uint32_t seed = 42);
    ~Arena();
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    bool fire(Ingredient first, Ingredient second);
    void update(const FrameTime& time);
    void reset(std::uint32_t seed = 42);
    void aim(float angle, float power);
    float angle() const;
    float power() const;
    // Predicted flight for at most `steps` fixed steps, stopping early at a hit.
    std::vector<Point> preview(int steps = 1920);
    std::size_t projectileCount() const;
    std::uint64_t shot() const;
    const Match& match() const;
    World& world();
    const World& world() const;
    Terrain& terrain();
    const Terrain& terrain() const;
    ObjectId penguin(std::size_t player) const;

private:
    GameObject* spawn(Point position, Point velocity, Potion potion, bool child);
    Point muzzle() const;
    Point velocity() const;
    bool trace(Point from, Point to, float radius, Point& hit, bool& ground) const;
    void requestImpact(GameObject& object, bool ground);
    void impact(const Event& event);
    void particles(Point position, Color color);
    void settle();

    EventManager& events_;
    World world_;
    World previewWorld_{1};
    Terrain terrain_{Width, Height, 560};
    Match match_;
    std::array<ObjectId, 2> penguins_{};
    std::array<float, 2> angles_{{45, 45}};
    std::array<float, 2> powers_{{600, 600}};
    EventManager::SubscriptionId impactSubscription_ = 0;
    std::uint64_t generation_ = 0;
    std::uint64_t shot_ = 0;
    double accumulator_ = 0;
    double elapsed_ = 0;
};
} // namespace Frostbrew
