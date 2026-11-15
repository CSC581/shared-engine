#pragma once

#include "Components.hpp"
#include "World.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace Neon {
enum class StorageMode { Heap, Pool, Reuse };
enum class Kind { Player, Guide, Enemy, Boss, PlayerShot, EnemyShot, Particle, Count };
constexpr std::size_t kindCount = static_cast<std::size_t>(Kind::Count);
using Capacities = std::array<std::size_t, kindCount>;
Capacities defaultCapacities();
const char* modeName(StorageMode mode);
struct Handle { std::size_t slot; std::uint64_t generation; };
struct Slot {
    Kind kind;
    GameObject* object = nullptr;
    Transform* body = nullptr;
    Motion* motion = nullptr;
    Renderable* render = nullptr;
    std::uint64_t generation = 0, birth = 0;
    bool used = false, retired = false;
    int health = 0;
    double age = 0, lifetime = 0;
};
struct StorageStats {
    std::uint64_t accepted = 0, rejected = 0, objectsConstructed = 0, componentsConstructed = 0;
    std::size_t active = 0, peakActive = 0;
};
class Storage {
public:
    explicit Storage(StorageMode mode = StorageMode::Reuse, Capacities capacities = defaultCapacities());
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;
    Storage(Storage&&) = delete;
    Storage& operator=(Storage&&) = delete;
    Handle spawn(Kind kind, Rect bounds, float vx = 0, float vy = 0, int health = 1, double lifetime = 0);
    Slot* get(Handle handle);
    const Slot* get(Handle handle) const;
    Handle handle(std::size_t slot) const;
    void retire(Handle handle);
    void beginStep();
    void rebuildGuide();
    World& world();
    const World& world() const;
    const std::vector<Slot>& slots() const;
    const StorageStats& stats() const;
    std::size_t capacity() const;
    std::size_t active(Kind kind) const;
    std::uint64_t digest() const;
    std::function<void(Handle, Handle)> onContact;
private:
    void construct(std::size_t index);
    StorageMode mode_;
    std::unique_ptr<World> world_;
    std::vector<Slot> slots_;
    StorageStats stats_;
    std::uint64_t serial_ = 0;
};
} // namespace Neon
