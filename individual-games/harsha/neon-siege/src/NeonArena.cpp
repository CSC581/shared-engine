#include "NeonArena.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>

namespace Neon {
Arena::Arena(StorageMode mode) : storage_(mode)
{
    hits_.reserve(256);
    storage_.onContact = [this](Handle self, Handle other) { contact(self, other); };
    player_ = storage_.spawn(Kind::Player, {464, 650, 32, 20});
    loadWave(false);
}
void Arena::loadWave(bool rebuild)
{
    if (rebuild) storage_.rebuildGuide();
    guide_ = storage_.spawn(Kind::Guide, {120, 110, 1, 1});
    direction_ = 1; formationY_ = 110; fireTicks_ = 0; spiral_ = 0;
    if (rules_.wave() == 3) storage_.spawn(Kind::Boss, {340, 100, 80, 40}, 0, 0, 40);
    else for (int row = 0; row < rules_.wave() + 1; ++row)
        for (int col = 0; col < 6; ++col)
            storage_.spawn(Kind::Enemy, {120.0F + col * 96, 110.0F + row * 44, 36, 24});
}
void Arena::contact(Handle self, Handle other)
{
    const auto* a = storage_.get(self); const auto* b = storage_.get(other);
    if (!a || !b) return;
    const bool hostile = a->kind == Kind::EnemyShot && b->kind == Kind::Player;
    const bool friendly = a->kind == Kind::PlayerShot && (b->kind == Kind::Enemy || b->kind == Kind::Boss);
    if (hostile || friendly) hits_.push_back({self, other, a->birth, hostile});
}
void Arena::burst(float x, float y)
{
    for (int i = 0; i < 8; ++i)
        storage_.spawn(Kind::Particle, {x, y, 3, 3}, (i - 3.5F) * 32, -60.0F + (i % 3) * 35, 1, 0.4);
}
void Arena::resolveHits()
{
    std::sort(hits_.begin(), hits_.end(), [](const Hit& a, const Hit& b) {
        return std::make_tuple(!a.hostile, a.birth, a.target.slot) < std::make_tuple(!b.hostile, b.birth, b.target.slot);
    });
    for (const auto& hit : hits_) {
        auto* bullet = storage_.get(hit.projectile); auto* target = storage_.get(hit.target);
        if (!bullet || !target) continue;
        const float x = bullet->body->x, y = bullet->body->y;
        storage_.retire(hit.projectile);
        if (hit.hostile) rules_.damage();
        else if (--target->health <= 0) {
            rules_.scoreHit(target->kind == Kind::Boss);
            storage_.retire(hit.target);
        }
        burst(x, y);
    }
    hits_.clear();
}
void Arena::enemyFire()
{
    ++fireTicks_;
    if (rules_.wave() < 3) {
        const int interval = rules_.wave() == 1 ? 288 : 240;
        if (fireTicks_ % interval != 0) return;
        const auto count = storage_.active(Kind::Enemy);
        if (count == 0) return;
        auto chosen = static_cast<std::size_t>(fireTicks_ / interval) % count;
        for (std::size_t i = 0; i < storage_.slots().size(); ++i) {
            const auto* enemy = storage_.get(storage_.handle(i));
            if (!enemy || enemy->kind != Kind::Enemy || chosen-- != 0) continue;
            const float x = enemy->body->x + 14, y = enemy->body->y + 24;
            storage_.spawn(Kind::EnemyShot, {x, y, 8, 10}, 0, 240, 1, 4);
            break;
        }
        return;
    }
    const auto pattern = rules_.pattern();
    const int interval = pattern == Pattern::Straight ? 36 : (pattern == Pattern::Spread ? 72 : 12);
    if (fireTicks_ % interval != 0) return;
    for (std::size_t i = 0; i < storage_.slots().size(); ++i) {
        const auto* boss = storage_.get(storage_.handle(i));
        if (!boss || boss->kind != Kind::Boss) continue;
        const float x = boss->body->x + 36, y = boss->body->y + 40;
        if (pattern == Pattern::Straight) storage_.spawn(Kind::EnemyShot, {x, y, 8, 10}, 0, 280, 1, 4);
        else if (pattern == Pattern::Spread) {
            for (int ray = -2; ray <= 2; ++ray)
                storage_.spawn(Kind::EnemyShot, {x, y, 8, 10}, ray * 70.0F, 240, 1, 4);
        } else {
            spiral_ += 0.28F;
            storage_.spawn(Kind::EnemyShot, {x, y, 8, 10}, std::cos(spiral_) * 280, std::sin(spiral_) * 280, 1, 4);
        }
        break;
    }
}
void Arena::step(Controls controls)
{
    storage_.beginStep();
    if (transition_) { loadWave(true); transition_ = false; }
    if (rules_.phase() != Phase::Playing) return;
    rules_.tick(); ++ticks_;
    if (shieldRequested_) { rules_.shield(); shieldRequested_ = false; }
    auto* player = storage_.get(player_);
    player->motion->velocityX = std::clamp(controls.horizontal, -1.0F, 1.0F) * 360;
    if (controls.fire && rules_.attemptShot())
        storage_.spawn(Kind::PlayerShot, {player->body->x + 12, player->body->y - 12, 8, 12}, 0, -600, 1, 2);
    storage_.world().updateComponents({Step, static_cast<std::int64_t>(ticks_ * 1000000 / 240)});
    player->body->x = std::clamp(player->body->x, 16.0F, Width - 48.0F);
    const auto* guide = storage_.get(guide_);
    const float vx = guide->motion->velocityX;
    const int direction = vx > 0.001F ? 1 : (vx < -0.001F ? -1 : direction_);
    if (direction != direction_ && rules_.wave() < 3) formationY_ += 14;
    direction_ = direction;
    int enemyIndex = 0;
    // Retain formation slots when enemies disappear; survivors must not shuffle.
    for (const auto& slot : storage_.slots()) {
        if (slot.kind == Kind::Enemy) {
            if (slot.used && !slot.retired) {
                slot.body->x = guide->body->x + (enemyIndex % 6) * 96;
                slot.body->y = formationY_ + (enemyIndex / 6) * 44;
                if (slot.body->y + slot.body->height >= 620) rules_.breach();
            }
            ++enemyIndex;
        } else if (slot.kind == Kind::Boss && slot.used && !slot.retired) {
            slot.body->x = guide->body->x + 220;
        }
    }
    if (rules_.phase() != Phase::Playing) return;
    enemyFire();
    storage_.world().resolveCollisions();
    resolveHits();
    for (std::size_t i = 0; i < storage_.slots().size(); ++i) {
        auto* slot = storage_.get(storage_.handle(i));
        if (!slot || slot->lifetime == 0) continue;
        slot->age += Step;
        const auto& b = *slot->body;
        if (slot->age + 1e-10 >= slot->lifetime || b.x < -32 || b.x > Width + 32 || b.y < -32 || b.y > Height + 32)
            storage_.retire(storage_.handle(i));
    }
    if (rules_.phase() == Phase::Playing && storage_.active(Kind::Enemy) + storage_.active(Kind::Boss) == 0) {
        rules_.clearWave();
        transition_ = rules_.phase() == Phase::Playing;
        for (std::size_t i = 0; i < storage_.slots().size(); ++i) {
            const auto& slot = storage_.slots()[i];
            if (slot.kind != Kind::Player && slot.kind != Kind::Guide) storage_.retire(storage_.handle(i));
        }
    }
}
void Arena::update(const FrameTime& time, Controls controls)
{
    if (!std::isfinite(time.dtSeconds) || time.dtSeconds < 0 || !std::isfinite(controls.horizontal))
        throw std::invalid_argument("Invalid Neon frame time or movement");
    if (rules_.phase() != Phase::Playing) return;
    accumulator_ += std::min(time.dtSeconds, 0.05);
    shieldRequested_ = shieldRequested_ || controls.shield;
    while (accumulator_ + 1e-10 >= Step) {
        accumulator_ = std::max(0.0, accumulator_ - Step);
        step(controls);
        if (rules_.phase() != Phase::Playing) break;
    }
}
const Rules& Arena::rules() const { return rules_; }
Storage& Arena::storage() { return storage_; }
const Storage& Arena::storage() const { return storage_; }
Handle Arena::player() const { return player_; }
std::uint64_t Arena::digest() const
{
    return storage_.digest() ^ (static_cast<std::uint64_t>(rules_.lives()) << 56) ^
        (static_cast<std::uint64_t>(rules_.wave()) << 48) ^ (static_cast<std::uint64_t>(rules_.phase()) << 40);
}
} // namespace Neon
