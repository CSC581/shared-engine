#include "NeonStorage.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; ++failures; } } while (false)
template<class F> void invalid(F f) { bool caught = false; try { f(); } catch (const std::invalid_argument&) { caught = true; } CHECK(caught); }
}
int main()
{
    using namespace Neon;
    CHECK(defaultCapacities()[static_cast<std::size_t>(Kind::PlayerShot)] == 24);
    invalid([] { Storage s(static_cast<StorageMode>(9)); });
    Capacities caps{}; caps[4] = 1; caps[6] = 1;
    for (auto mode : {StorageMode::Heap, StorageMode::Pool, StorageMode::Reuse}) {
        Storage s(mode, caps);
        CHECK(s.capacity() == 2);
        auto h = s.spawn(Kind::PlayerShot, {1, 2, 8, 12}, 3, -4, 1, 2);
        auto* slot = s.get(h); CHECK(slot != nullptr);
        if (!slot) continue;
        CHECK(slot->object->isActive());
        const auto id = slot->object->id();
        slot->motion->velocityX = 99; slot->health = -10;
        slot->render->visible = false; slot->age = 90;
        slot->object->get<Collider>()->layers = 0;
        s.retire(h); s.retire(h);
        CHECK(s.get(h) == nullptr);
        CHECK(s.get(s.spawn(Kind::PlayerShot, {1, 2, 8, 12})) == nullptr);
        s.beginStep();
        auto next = s.spawn(Kind::PlayerShot, {10, 20, 8, 12}, 0, -600, 1, 2);
        slot = s.get(next); CHECK(slot != nullptr);
        if (!slot) continue;
        CHECK(next.generation != h.generation && s.get(h) == nullptr);
        CHECK(slot->age == 0 && slot->health == 1 && slot->render->visible);
        CHECK(slot->body->x == 10 && slot->motion->velocityX == 0 && slot->motion->velocityY == -600);
        CHECK(slot->object->get<Collider>()->layers == 1);
        CHECK((slot->object->id() == id) == (mode == StorageMode::Reuse));
        CHECK(s.get(s.spawn(Kind::EnemyShot, {0, 0, 8, 8})) == nullptr);
        CHECK(s.get(s.spawn(Kind::Particle, {0, 0, 3, 3})) != nullptr);
        CHECK(s.get(s.spawn(Kind::Particle, {0, 0, 3, 3})) == nullptr);
        CHECK(s.stats().active == 2 && s.stats().peakActive == 2);
        invalid([&] { s.spawn(Kind::PlayerShot, {0, 0, 0, 1}); });
        invalid([&] { s.spawn(Kind::PlayerShot, {0, 0, 1, 1}, std::numeric_limits<float>::infinity()); });
    }
    Capacities parityCaps{}; parityCaps[4] = 3;
    Storage heap(StorageMode::Heap, parityCaps), pool(StorageMode::Pool, parityCaps), reuse(StorageMode::Reuse, parityCaps);
    for (int tick = 0; tick < 400; ++tick) {
        for (auto* s : {&heap, &pool, &reuse}) {
            s->beginStep();
            for (std::size_t i = 0; i < s->slots().size(); ++i) {
                auto* slot = s->get(s->handle(i));
                if (slot && tick % 3 == 0) s->retire(s->handle(i));
            }
            s->spawn(Kind::PlayerShot, {float(tick), 0, 8, 12}, 0, 1);
            s->world().updateComponents({1.0 / 240, tick * 4167LL});
        }
        CHECK(heap.digest() == pool.digest() && heap.digest() == reuse.digest());
    }
    CHECK(reuse.stats().objectsConstructed == 3);
    CHECK(heap.stats().objectsConstructed > reuse.stats().objectsConstructed);
    std::cout << "Neon storage: " << failures << " failures\n";
    return failures != 0;
}
