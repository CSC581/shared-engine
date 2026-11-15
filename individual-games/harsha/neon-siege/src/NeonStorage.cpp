#include "NeonStorage.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace Neon {
namespace {
class Actor final : public Component {
public:
    explicit Actor(std::size_t index) : index(index) {}
    std::size_t index;
};
bool moving(Kind k) { return k != Kind::Enemy && k != Kind::Boss; }
bool colliding(Kind k) { return k != Kind::Guide && k != Kind::Particle; }
std::uint32_t layers(Kind k) { return k == Kind::Player || k == Kind::EnemyShot ? 2U : 1U; }
Color color(Kind k)
{
    switch (k) {
    case Kind::Player: return {70, 240, 255};
    case Kind::Enemy: return {240, 80, 200};
    case Kind::Boss: return {255, 145, 55};
    case Kind::PlayerShot: return {150, 255, 240};
    case Kind::EnemyShot: return {255, 95, 110};
    default: return {160, 150, 255};
    }
}
}
Capacities defaultCapacities() { return {{1, 1, 18, 1, 24, 128, 64}}; }
const char* modeName(StorageMode mode)
{
    switch (mode) {
    case StorageMode::Heap: return "heap";
    case StorageMode::Pool: return "pool";
    case StorageMode::Reuse: return "reuse";
    }
    throw std::invalid_argument("Unknown storage mode");
}
Storage::Storage(StorageMode mode, Capacities capacities) : mode_(mode)
{
    modeName(mode);
    std::size_t total = 0;
    for (auto count : capacities) {
        if (count > 4096 - total) throw std::invalid_argument("Neon storage exceeds 4096 objects");
        total += count;
    }
    if (total == 0) throw std::invalid_argument("Neon storage needs at least one slot");
    world_ = mode == StorageMode::Heap ? std::make_unique<World>() : std::make_unique<World>(total);
    slots_.reserve(total);
    for (std::size_t kind = 0; kind < kindCount; ++kind)
        for (std::size_t i = 0; i < capacities[kind]; ++i) {
            Slot slot{}; slot.kind = static_cast<Kind>(kind); slots_.push_back(slot);
        }
    if (mode == StorageMode::Reuse)
        for (std::size_t i = 0; i < slots_.size(); ++i) construct(i);
}
void Storage::construct(std::size_t index)
{
    auto& slot = slots_[index];
    auto& object = world_->create("neon");
    slot.object = &object;
    slot.body = &object.add<Transform>(0, 0, 1, 1);
    if (moving(slot.kind)) slot.motion = &object.add<Motion>();
    if (slot.kind == Kind::Guide)
        object.add<PathMover>(PathMover::Linear{120, 110, 320, 110}, 90);
    if (slot.kind == Kind::Particle) object.add<Gravity>(120);
    if (colliding(slot.kind)) {
        auto& collider = object.add<Collider>(Collider::Kind::Trigger, layers(slot.kind));
        collider.onCollide = [this, index](GameObject&, GameObject& other) {
            const auto* actor = other.get<Actor>();
            if (actor && onContact) onContact(handle(index), handle(actor->index));
        };
    }
    if (slot.kind != Kind::Guide) slot.render = &object.add<Renderable>(color(slot.kind));
    object.add<Actor>(index);
    ++stats_.objectsConstructed;
    stats_.componentsConstructed += object.components().size();
}
Handle Storage::spawn(Kind kind, Rect bounds, float vx, float vy, int health, double lifetime)
{
    if (static_cast<std::size_t>(kind) >= kindCount || !std::isfinite(bounds.x) || !std::isfinite(bounds.y) ||
        !std::isfinite(bounds.width) || !std::isfinite(bounds.height) || bounds.width <= 0 || bounds.height <= 0 ||
        !std::isfinite(vx) || !std::isfinite(vy) || !std::isfinite(lifetime) || lifetime < 0 || health <= 0)
        throw std::invalid_argument("Invalid Neon spawn");
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        auto& slot = slots_[i];
        if (slot.kind != kind || slot.used) continue;
        if (!slot.object) construct(i);
        slot.body->x = bounds.x; slot.body->y = bounds.y;
        slot.body->width = bounds.width; slot.body->height = bounds.height;
        if (slot.motion) { slot.motion->velocityX = vx; slot.motion->velocityY = vy; slot.motion->kinematic = kind == Kind::Guide; }
        if (auto* collider = slot.object->get<Collider>()) {
            collider->layers = layers(kind); collider->kind = Collider::Kind::Trigger;
        }
        if (slot.render) {
            slot.render->visible = true; slot.render->color = color(kind);
            slot.render->texture = nullptr;
            slot.render->layer = kind == Kind::Particle ? 0 : (kind == Kind::Player ? 3 : 2);
        }
        if (auto* gravity = slot.object->get<Gravity>()) gravity->acceleration = 120;
        slot.health = health; slot.age = 0; slot.lifetime = lifetime;
        slot.used = true; slot.retired = false; ++slot.generation; slot.birth = ++serial_;
        slot.object->setActive(true);
        ++stats_.accepted; ++stats_.active;
        stats_.peakActive = std::max(stats_.peakActive, stats_.active);
        return handle(i);
    }
    ++stats_.rejected;
    return {slots_.size(), 0};
}
Slot* Storage::get(Handle h)
{
    if (h.slot >= slots_.size()) return nullptr;
    auto& slot = slots_[h.slot];
    return slot.used && !slot.retired && slot.generation == h.generation ? &slot : nullptr;
}
const Slot* Storage::get(Handle h) const { return const_cast<Storage*>(this)->get(h); }
Handle Storage::handle(std::size_t i) const { return {i, i < slots_.size() ? slots_[i].generation : 0}; }
void Storage::retire(Handle h)
{
    if (auto* slot = get(h)) {
        slot->retired = true; slot->object->setActive(false); --stats_.active;
    }
}
void Storage::beginStep()
{
    for (auto& slot : slots_) if (slot.retired) {
        slot.used = false; slot.retired = false;
        if (mode_ != StorageMode::Reuse) {
            world_->destroy(slot.object->id());
            slot.object = nullptr; slot.body = nullptr; slot.motion = nullptr; slot.render = nullptr;
        }
    }
    world_->flushDestroyed();
}
void Storage::rebuildGuide()
{
    for (auto& slot : slots_) if (slot.kind == Kind::Guide && slot.object) {
        if (slot.used && !slot.retired) --stats_.active;
        world_->destroy(slot.object->id());
        slot.object = nullptr; slot.body = nullptr; slot.motion = nullptr; slot.render = nullptr;
        slot.used = false; slot.retired = false;
    }
    world_->flushDestroyed();
}
World& Storage::world() { return *world_; }
const World& Storage::world() const { return *world_; }
const std::vector<Slot>& Storage::slots() const { return slots_; }
const StorageStats& Storage::stats() const { return stats_; }
std::size_t Storage::capacity() const { return slots_.size(); }
std::size_t Storage::active(Kind kind) const
{
    return static_cast<std::size_t>(std::count_if(slots_.begin(), slots_.end(), [kind](const Slot& s) {
        return s.kind == kind && s.used && !s.retired;
    }));
}
std::uint64_t Storage::digest() const
{
    std::uint64_t hash = 1469598103934665603ULL;
    const auto add = [&hash](std::uint64_t value) { hash ^= value; hash *= 1099511628211ULL; };
    add(stats_.accepted); add(stats_.rejected);
    for (const auto& slot : slots_) if (slot.used && !slot.retired) {
        add(static_cast<std::uint64_t>(slot.kind)); add(slot.birth); add(slot.health);
        for (float value : {slot.body->x, slot.body->y, slot.body->width, slot.body->height,
                            slot.motion ? slot.motion->velocityX : 0, slot.motion ? slot.motion->velocityY : 0}) {
            std::uint32_t bits; std::memcpy(&bits, &value, sizeof bits); add(bits);
        }
    }
    return hash;
}
} // namespace Neon
