#include "NeonArena.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; ++failures; } } while (false)
template<class F> void invalid(F f) { bool caught = false; try { f(); } catch (const std::invalid_argument&) { caught = true; } CHECK(caught); }
void tick(Neon::Arena& a) { a.update({Neon::Arena::Step, 0}); }
void eliminate(Neon::Arena& a)
{
    auto& s = a.storage();
    for (std::size_t i = 0; i < s.slots().size(); ++i) {
        auto* target = s.get(s.handle(i));
        if (!target || (target->kind != Neon::Kind::Enemy && target->kind != Neon::Kind::Boss)) continue;
        target->health = 1;
        s.spawn(Neon::Kind::PlayerShot, target->body->bounds(), 0, 0, 1, 2);
    }
    tick(a);
}
}
int main()
{
    using namespace Neon;
    Arena a;
    CHECK(a.storage().capacity() == 237);
    CHECK(a.storage().active(Kind::Enemy) == 12);
    const auto initial = a.digest();
    a.update({0, 0}); CHECK(a.digest() == initial);
    invalid([&] { a.update({-1, 0}); });
    invalid([&] { a.update({std::numeric_limits<double>::quiet_NaN(), 0}); });
    invalid([&] { a.update({0, 0}, {std::numeric_limits<float>::infinity()}); });
    Arena single, split;
    single.update({1.0 / 30, 0}, {1, true});
    split.update({1.0 / 60, 0}, {1, true}); split.update({1.0 / 60, 0}, {1, true});
    CHECK(single.digest() == split.digest());
    auto& s = a.storage();
    auto h = s.spawn(Kind::EnemyShot, s.get(a.player())->body->bounds(), 0, 0, 1, 2);
    tick(a); CHECK(a.rules().lives() == 2); CHECK(s.get(h) == nullptr);
    const auto slotIndex = h.slot;
    for (int i = 0; i < 240; ++i) tick(a);
    h = s.spawn(Kind::EnemyShot, s.get(a.player())->body->bounds(), 0, 0, 1, 2);
    CHECK(h.slot == slotIndex);
    tick(a); CHECK(a.rules().lives() == 1);
    a.update({Arena::Step, 0}, {0, false, true});
    s.spawn(Kind::EnemyShot, s.get(a.player())->body->bounds(), 0, 0, 1, 2);
    tick(a); CHECK(a.rules().lives() == 1);
    Arena overlap;
    auto& os = overlap.storage();
    Slot* first = nullptr; Slot* second = nullptr;
    for (std::size_t i = 0; i < os.slots().size(); ++i) {
        auto* slot = os.get(os.handle(i));
        if (slot && slot->kind == Kind::Enemy) { if (!first) first = slot; else if (!second) second = slot; }
    }
    CHECK(first && second);
    if (first && second) {
        // A wide projectile can overlap two enemies during one collected collision pass.
        os.spawn(Kind::PlayerShot, {first->body->x, first->body->y, 160, 24}, 0, 0, 1, 2);
        tick(overlap); CHECK(os.active(Kind::Enemy) == 11); CHECK(overlap.rules().score() == 10);
    }
    Arena win;
    eliminate(win); CHECK(win.rules().wave() == 2); tick(win);
    CHECK(win.storage().active(Kind::Enemy) == 18);
    eliminate(win); CHECK(win.rules().wave() == 3); tick(win);
    CHECK(win.storage().active(Kind::Boss) == 1);
    eliminate(win); CHECK(win.rules().phase() == Phase::Won);
    Arena heap(StorageMode::Heap), pool(StorageMode::Pool), reuse;
    for (int i = 0; i < 2400; ++i) {
        Controls controls{float((i / 360) % 3 - 1), true, i % 720 == 0};
        for (auto* arena : {&heap, &pool, &reuse}) arena->update({Arena::Step, 0}, controls);
        CHECK(heap.digest() == pool.digest() && heap.digest() == reuse.digest());
    }
    std::cout << "Neon arena: " << failures << " failures\n";
    return failures != 0;
}
