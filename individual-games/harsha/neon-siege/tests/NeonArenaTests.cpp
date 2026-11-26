#include "NeonArena.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>

static_assert(!std::is_move_constructible_v<Neon::Arena>, "Contact callbacks require stable arena ownership");

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
    Arena expiry;
    auto expired = expiry.storage().spawn(Kind::PlayerShot, {10, 300, 8, 12}, 0, 0, 1, Arena::Step);
    tick(expiry); CHECK(expiry.storage().get(expired) == nullptr);
    tick(expiry);
    auto outside = expiry.storage().spawn(Kind::EnemyShot, {-40, 300, 8, 10}, 0, 0, 1, 2);
    tick(expiry); CHECK(expiry.storage().get(outside) == nullptr);
    Arena simultaneous;
    for (int hit = 0; hit < 2; ++hit) {
        simultaneous.storage().spawn(Kind::EnemyShot, simultaneous.storage().get(simultaneous.player())->body->bounds(), 0, 0, 1, 2);
        tick(simultaneous);
        for (int i = 0; i < 240; ++i) tick(simultaneous);
    }
    CHECK(simultaneous.rules().lives() == 1);
    eliminate(simultaneous); tick(simultaneous);
    eliminate(simultaneous); tick(simultaneous);
    simultaneous.storage().spawn(Kind::EnemyShot, simultaneous.storage().get(simultaneous.player())->body->bounds(), 0, 0, 1, 2);
    eliminate(simultaneous); CHECK(simultaneous.rules().phase() == Phase::Lost);
    for (auto mode : {StorageMode::Heap, StorageMode::Pool, StorageMode::Reuse}) {
        Arena consecutive(mode);
        eliminate(consecutive); tick(consecutive); eliminate(consecutive); tick(consecutive);
        auto& cs = consecutive.storage();
        for (std::size_t i = 0; i < cs.slots().size(); ++i) {
            auto* boss = cs.get(cs.handle(i));
            if (!boss || boss->kind != Kind::Boss) continue;
            boss->health = 2;
            auto shot = cs.spawn(Kind::PlayerShot, boss->body->bounds(), 0, 0, 1, 2);
            tick(consecutive); CHECK(boss->health == 1);
            // The next pass has the same reused IDs and no missing-contact pass.
            cs.beginStep();
            auto next = cs.spawn(Kind::PlayerShot, boss->body->bounds(), 0, 0, 1, 2);
            CHECK(next.slot == shot.slot && next.generation != shot.generation);
            CHECK(cs.get(shot) == nullptr);
            tick(consecutive); CHECK(consecutive.rules().phase() == Phase::Won);
        }
    }
    Arena bossDamage;
    eliminate(bossDamage); tick(bossDamage); eliminate(bossDamage); tick(bossDamage);
    for (int hit = 0; hit < 40; ++hit) {
        auto& bs = bossDamage.storage();
        bs.beginStep();
        for (std::size_t i = 0; i < bs.slots().size(); ++i) {
            auto* boss = bs.get(bs.handle(i));
            if (boss && boss->kind == Kind::Boss)
                bs.spawn(Kind::PlayerShot, boss->body->bounds(), 0, 0, 1, 2);
        }
        tick(bossDamage);
        CHECK(bossDamage.rules().phase() == (hit == 39 ? Phase::Won : Phase::Playing));
    }
    Arena reversal;
    for (int i = 0; i < 600; ++i) tick(reversal);
    for (std::size_t i = 0; i < reversal.storage().slots().size(); ++i) {
        const auto* enemy = reversal.storage().get(reversal.storage().handle(i));
        if (enemy && enemy->kind == Kind::Enemy) { CHECK(enemy->body->y >= 124); break; }
    }
    Arena heap(StorageMode::Heap), pool(StorageMode::Pool), reuse;
    for (int i = 0; i < 2400; ++i) {
        Controls controls{float((i / 360) % 3 - 1), true, i % 720 == 0};
        for (auto* arena : {&heap, &pool, &reuse}) arena->update({Arena::Step, 0}, controls);
        CHECK(heap.digest() == pool.digest() && heap.digest() == reuse.digest());
        CHECK(heap.rules().score() == reuse.rules().score() && pool.rules().score() == reuse.rules().score());
        CHECK(heap.rules().shieldCooldown() == reuse.rules().shieldCooldown());
        CHECK(pool.rules().protectedNow() == reuse.rules().protectedNow());
        CHECK(heap.rules().pattern() == reuse.rules().pattern());
    }
    for (int wave = 0; wave < 3; ++wave) {
        for (auto* arena : {&heap, &pool, &reuse}) { eliminate(*arena); tick(*arena); }
        CHECK(heap.digest() == pool.digest() && heap.digest() == reuse.digest());
        CHECK(heap.rules().score() == reuse.rules().score());
    }
    CHECK(reuse.rules().phase() == Phase::Won);
    std::cout << "Neon arena: " << failures << " failures\n";
    return failures != 0;
}
