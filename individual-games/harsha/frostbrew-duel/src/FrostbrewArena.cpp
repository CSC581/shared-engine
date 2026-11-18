#include "FrostbrewArena.hpp"

#include "Components.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Frostbrew {
namespace {
constexpr float gravity = 400;
constexpr float radians = 3.14159265358979323846F / 180;
constexpr const char* impactType = "Frostbrew.Impact";
constexpr std::array<Color, 6> colors{{
    {255, 136, 68}, {220, 185, 255}, {255, 190, 130},
    {150, 160, 195}, {100, 225, 150}, {180, 240, 110}
}};

class Projectile final : public Component {
public:
    Projectile(Potion potion, bool child, double born) : potion(potion), child(child), born(born) {}
    Potion potion;
    bool child;
    double born;
    bool spent = false;
};
} // namespace

Arena::Arena(EventManager& events, std::size_t capacity, std::uint32_t seed)
    : events_(events), world_(capacity)
{
    if (capacity < 2) throw std::invalid_argument("Frostbrew needs two penguin slots");
    auto& prediction = previewWorld_.create("prediction");
    prediction.add<Transform>(0, 0, 10, 10);
    prediction.add<Motion>();
    prediction.add<Gravity>(gravity);
    prediction.setActive(true);
    world_.setEventManager(&events_);
    reset(seed);
    impactSubscription_ = events_.subscribe(impactType, [this](const Event& event) { impact(event); });
}
Arena::~Arena()
{
    events_.unsubscribe(impactSubscription_);
}
void Arena::reset(std::uint32_t seed)
{
    ++generation_;
    for (const auto& object : world_.objects()) world_.destroy(object->id());
    world_.flushDestroyed();
    match_.reset();
    angles_.fill(45); powers_.fill(600);
    accumulator_ = 0;
    elapsed_ = 0;
    terrain_.generate(seed);
    for (std::size_t player = 0; player < 2; ++player) {
        auto& object = world_.create("penguin");
        object.add<Transform>(player == 0 ? 180.0F : 988.0F, 0, 32, 32);
        object.add<Collider>(Collider::Kind::Trigger);
        object.setActive(true);
        penguins_[player] = object.id();
    }
    settle();
    recordPoolUsage();
}
void Arena::settle()
{
    for (auto id : penguins_) {
        auto* body = world_.find(id)->get<Transform>();
        body->y = terrain_.support(body->x, body->width) - body->height;
    }
}
void Arena::aim(float angle, float power)
{
    if (!std::isfinite(angle) || !std::isfinite(power)) throw std::invalid_argument("Invalid aim");
    if (match_.phase() != Phase::Aim) return;
    angles_[match_.player()] = std::clamp(angle, 5.0F, 85.0F);
    powers_[match_.player()] = std::clamp(power, 200.0F, 700.0F);
}
float Arena::angle() const { return angles_[match_.player()]; }
float Arena::power() const { return powers_[match_.player()]; }
Point Arena::velocity() const
{
    const float direction = match_.player() == 0 ? 1.0F : -1.0F;
    return {direction * std::cos(angle() * radians) * power(), -std::sin(angle() * radians) * power()};
}
Point Arena::muzzle() const
{
    const auto* body = world_.find(penguins_[match_.player()])->get<Transform>();
    const Point speed = velocity();
    return {body->x + body->width / 2 + speed.x / power() * 32,
            body->y + body->height / 2 + speed.y / power() * 32};
}
GameObject* Arena::spawn(Point position, Point speed, Potion potion, bool child)
{
    auto* object = world_.tryCreate("flask");
    if (!object) {
        ++(child ? poolUsage_.secondaryRejected : poolUsage_.primaryRejected);
        return nullptr;
    }
    try {
        object->add<Transform>(position.x - 5, position.y - 5, 10, 10);
        object->add<Motion>(speed.x, speed.y);
        object->add<Gravity>(gravity);
        object->add<Collider>(Collider::Kind::Trigger).onEnter = [this](GameObject& self, GameObject& other) {
            if (other.tag() == "penguin") requestImpact(self, false);
        };
        object->add<Renderable>(colors[static_cast<unsigned>(potion)]).layer = 2;
        object->add<Projectile>(potion, child, elapsed_);
        object->setActive(true);
        ++(child ? poolUsage_.secondarySpawned : poolUsage_.primarySpawned);
        recordPoolUsage();
    } catch (...) {
        world_.destroy(object->id());
        world_.flushDestroyed();
        throw;
    }
    return object;
}
bool Arena::fire(Ingredient first, Ingredient second)
{
    const auto potion = recipe(first, second).potion;
    if (match_.phase() != Phase::Aim) return false;
    auto* object = spawn(muzzle(), velocity(), potion, false);
    if (!object) return false;
    ++shot_;
    accumulator_ = 0;
    return match_.beginShot(potion, true);
}
bool Arena::trace(Point from, Point to, float radius, Point& hit, bool& ground) const
{
    const int samples = std::max(1, static_cast<int>(std::ceil(std::max(std::abs(to.x - from.x),
                                                                           std::abs(to.y - from.y)) * 2)));
    for (int i = 0; i <= samples; ++i) {
        const float t = static_cast<float>(i) / samples;
        const Point p{from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t};
        const Rect bounds{p.x - radius, p.y - radius, radius * 2, radius * 2};
        for (const auto id : penguins_) {
            const auto* body = world_.find(id)->get<Transform>();
            if (Collision::intersects(bounds, body->bounds())) {
                hit = p; ground = false; return true;
            }
        }
        if (terrain_.touches(p.x, p.y, radius)) {
            hit = p; ground = true; return true;
        }
    }
    return false;
}
void Arena::requestImpact(GameObject& object, bool ground)
{
    auto* projectile = object.get<Projectile>();
    if (!projectile || projectile->spent) return;
    projectile->spent = true;
    object.setActive(false);
    Event event(impactType);
    event.priority = EventPriority::High;
    event.set("object", object.id()).set("shot", shot_).set("generation", generation_).set("ground", ground);
    events_.raise(std::move(event));
}
void Arena::impact(const Event& event)
{
    if (event.origin != 0 || match_.phase() != Phase::Resolving ||
        event.getOr<std::int64_t>("generation", -1) != static_cast<std::int64_t>(generation_) ||
        event.getOr<std::int64_t>("shot", -1) != static_cast<std::int64_t>(shot_)) return;
    auto* object = world_.find(static_cast<ObjectId>(event.get<std::int64_t>("object")));
    if (!object) return;
    auto* projectile = object->get<Projectile>();
    if (!projectile || !projectile->spent) return;
    // Remove its effect authority before another queued event can see the slot.
    projectile->spent = false;
    const Potion potion = projectile->potion;
    const bool split = !projectile->child && (potion == Potion::Shatter || potion == Potion::Bloom);
    const auto* transform = object->get<Transform>();
    Point position{transform->x + 5, transform->y + 5};
    if (event.getOr<bool>("ground", false)) position.y = terrain_.support(position.x - 5, 10);
    world_.destroy(object->id());
    const auto& effect = recipe(potion);
    if (!split) {
        terrain_.edit(position.x, position.y, effect.width, effect.depth, effect.growth);
        for (std::size_t player = 0; player < 2; ++player) {
            const auto* target = world_.find(penguins_[player])->get<Transform>();
            const float dx = position.x - std::clamp(position.x, target->x, target->x + target->width);
            const float dy = position.y - std::clamp(position.y, target->y, target->y + target->height);
            const float falloff = std::max(0.0F, 1 - std::hypot(dx, dy) / std::max(48.0F, effect.width));
            match_.damage(player, static_cast<int>(std::ceil(effect.damage * falloff)));
        }
        settle();
    }
    world_.flushDestroyed();
    if (split) {
        for (int i = 0; i < 8; ++i) {
            const float angle = (20 + i * 20) * radians;
            spawn({position.x, position.y - 12}, {std::cos(angle) * 230, -std::sin(angle) * 230}, potion, true);
        }
    }
    particles(position, colors[static_cast<unsigned>(potion)]);
}
void Arena::particles(Point position, Color color)
{
    std::size_t count = world_.findAllByTag("particle").size();
    for (int i = 0; i < 12; ++i, ++count) {
        if (count >= 64) { poolUsage_.particleLimitSkipped += 12 - i; break; }
        // Keep nine slots available for a primary flask and its children.
        if (world_.objects().size() + 9 >= world_.objectCapacity()) {
            poolUsage_.particleReserveSkipped += 12 - i;
            break;
        }
        auto* object = world_.tryCreate("particle");
        if (!object) { poolUsage_.particleRejected += 12 - i; break; }
        object->add<Transform>(position.x, position.y, 3, 3);
        object->add<Motion>((i - 5.5F) * 28, -70.0F - (i % 3) * 35);
        object->add<Gravity>(gravity);
        object->add<Renderable>(color).layer = 1;
        object->add<Behavior>([this, life = 0.65](GameObject& self, const FrameTime& time) mutable {
            life -= time.dtSeconds;
            if (life <= 0) world_.destroy(self.id());
        });
        object->setActive(true);
        ++poolUsage_.particlesSpawned;
        recordPoolUsage();
    }
}
void Arena::recordPoolUsage()
{
    std::size_t projectiles = 0, particles = 0;
    for (const auto& object : world_.objects()) {
        projectiles += object->has<Projectile>();
        particles += object->tag() == "particle";
    }
    poolUsage_.peakObjects = std::max(poolUsage_.peakObjects, world_.objects().size());
    poolUsage_.peakProjectiles = std::max(poolUsage_.peakProjectiles, projectiles);
    poolUsage_.peakParticles = std::max(poolUsage_.peakParticles, particles);
}
const PoolUsage& Arena::poolUsage() const { return poolUsage_; }
void Arena::update(const FrameTime& time)
{
    if (!std::isfinite(time.dtSeconds) || time.dtSeconds < 0) throw std::invalid_argument("Invalid frame time");
    accumulator_ += std::min(time.dtSeconds, 0.05);
    while (accumulator_ + 1e-10 >= Step) {
        accumulator_ = std::max(0.0, accumulator_ - Step);
        elapsed_ += Step;
        std::vector<std::pair<ObjectId, Point>> previous;
        for (const auto& object : world_.objects()) if (object->isActive() && object->has<Projectile>()) {
            const auto* body = object->get<Transform>();
            previous.push_back({object->id(), {body->x + 5, body->y + 5}});
        }
        world_.updateComponents(FrameTime{Step, static_cast<std::int64_t>(elapsed_ * 1000000)});
        for (const auto& [id, from] : previous) {
            auto* object = world_.find(id);
            auto* body = object->get<Transform>();
            const Point end{body->x + 5, body->y + 5};
            Point hit{}; bool ground = false;
            if (trace(from, end, 5, hit, ground)) {
                body->x = hit.x - 5; body->y = hit.y - 5;
                if (ground) requestImpact(*object, true);
            } else if (end.x < -10 || end.x > Width + 10 || elapsed_ - object->get<Projectile>()->born >= 8) {
                object->setActive(false);
                world_.destroy(id);
            }
        }
        world_.resolveCollisions();
        events_.dispatch();
        world_.flushDestroyed();
        if (match_.phase() == Phase::Resolving && projectileCount() == 0) {
            settle();
            match_.finishShot();
        }
    }
}
std::vector<Point> Arena::preview()
{
    auto* prediction = previewWorld_.findByTag("prediction");
    auto* body = prediction->get<Transform>();
    auto* motion = prediction->get<Motion>();
    Point previous = muzzle();
    body->x = previous.x - 5; body->y = previous.y - 5;
    const auto speed = velocity();
    motion->velocityX = speed.x; motion->velocityY = speed.y;
    std::vector<Point> path{previous};
    for (int i = 0; i < 1920; ++i) {
        previewWorld_.updateComponents(FrameTime{Step, static_cast<std::int64_t>((i + 1) * Step * 1000000)});
        Point current{body->x + 5, body->y + 5}, hit{}; bool ground = false;
        if (trace(previous, current, 5, hit, ground)) { path.push_back(hit); break; }
        path.push_back(current);
        if (current.x < -10 || current.x > Width + 10) break;
        previous = current;
    }
    return path;
}
std::size_t Arena::projectileCount() const
{
    std::size_t count = 0;
    for (const auto& object : world_.objects()) if (object->has<Projectile>()) ++count;
    return count;
}
std::uint64_t Arena::shot() const { return shot_; }
const Match& Arena::match() const { return match_; }
World& Arena::world() { return world_; }
const World& Arena::world() const { return world_; }
Terrain& Arena::terrain() { return terrain_; }
const Terrain& Arena::terrain() const { return terrain_; }
ObjectId Arena::penguin(std::size_t player) const
{
    if (player >= 2) throw std::invalid_argument("Invalid player");
    return penguins_[player];
}
} // namespace Frostbrew
