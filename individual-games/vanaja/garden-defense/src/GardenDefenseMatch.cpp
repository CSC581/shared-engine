#include "GardenDefenseMatch.hpp"

#include "TimeUnits.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>

namespace GardenDefense {

namespace {

constexpr Color dropColor{90, 170, 255};
constexpr Color seedColor{196, 150, 82};
constexpr Color ladybugColor{225, 55, 55};
constexpr std::array<Color, 4> petalColors{{
    {250, 140, 190}, {255, 220, 90}, {255, 255, 255}, {190, 150, 255},
}};
constexpr float marchFrameSeconds = 0.25F;
constexpr float twoPi = 6.2831853F;

std::int64_t ticsFor(float seconds)
{
    return static_cast<std::int64_t>(static_cast<double>(seconds) * kGameTicsPerSecond);
}

} // namespace

void Lifetime::update(const FrameTime& time)
{
    remaining_ -= static_cast<float>(time.dtSeconds);
    if (remaining_ <= 0.0F && owner().isActive()) {
        owner().setActive(false);
        world_.destroy(owner().id());
    }
}

Match::Match(EventManager& events, std::uint32_t seed)
    : events_(events), rng_(seed)
{
    world_.setEventManager(&events_);

    // The can starts inactive; the first SpawnEvent places and activates it.
    can_ = &world_.create("can");
    canTransform_ = &can_->add<Transform>((width - canWidth) * 0.5F, canY, canWidth, canHeight);
    canMotion_ = &can_->add<Motion>();
    can_->add<Collider>(Collider::Kind::Trigger, seedLayer);

    // The garden fence is a hidden trigger; the game draws the fence itself.
    fence_ = &world_.create("fence");
    fence_->add<Transform>(0.0F, gardenLineY, static_cast<float>(width), 4.0F);
    fence_->add<Collider>(Collider::Kind::Trigger, gardenLayer);
    fence_->setActive(true);

    subscriptions_.push_back(events_.subscribe(Events::Collision, [this](const Event& event) { onCollision(event); }));
    subscriptions_.push_back(events_.subscribe(Events::Death, [this](const Event& event) { onDeath(event); }));
    subscriptions_.push_back(events_.subscribe(Events::Spawn, [this](const Event& event) { onSpawn(event); }));
    restart();
}

Match::~Match()
{
    for (const EventManager::SubscriptionId id : subscriptions_) {
        events_.unsubscribe(id);
    }
}

void Match::restart()
{
    ++generation_;
    for (const auto& object : world_.objects()) {
        if (object.get() != can_ && object.get() != fence_) {
            world_.destroy(object->id());
        }
    }
    // Frees the old match's slots now, before the new formation needs them.
    world_.flushDestroyed();
    can_->setActive(false);

    state_ = State::Playing;
    wave_ = 1;
    score_ = 0;
    lives_ = startingLives;
    shield_ = 0.0F;
    umbrella_ = 0.0F;
    cooldown_ = 0.0F;
    blocked_ = 0;
    skipped_ = 0;
    ladybugTimer_ = ladybugIntervalMin + (ladybugIntervalMax - ladybugIntervalMin) * roll();

    const auto generation = static_cast<std::int64_t>(generation_);
    events_.raise(Event(Events::Spawn).set("kind", "wave").set("wave", 1).set("generation", generation));
    Event can(Events::Spawn);
    can.set("kind", "can").set("generation", generation);
    can.priority = EventPriority::High;
    events_.raise(std::move(can));
}

void Match::update(const FrameTime& time)
{
    const float dt = static_cast<float>(time.dtSeconds);
    if (dt > 0.0F) {
        shield_ = std::max(0.0F, shield_ - dt);
        umbrella_ = std::max(0.0F, umbrella_ - dt);
        cooldown_ = std::max(0.0F, cooldown_ - dt);
        if (state_ == State::Playing) {
            march(dt);
            dropSeed(dt);
            sendLadybug(dt);
        }
    }

    const bool canMove = can_->isActive() && (state_ == State::Playing || state_ == State::WaveCleared);
    canMotion_->velocityX = canMove ? axis_ * canSpeed : 0.0F;
    world_.update(time);
    canTransform_->x = std::clamp(canTransform_->x, edgeMargin, width - edgeMargin - canWidth);
    cullStray();
}

void Match::setMove(float axis)
{
    axis_ = std::clamp(axis, -1.0F, 1.0F);
}

bool Match::fire()
{
    if (!can_->isActive() || state_ == State::Lost || state_ == State::Won ||
        countActive("drop") >= maxDrops) {
        return false;
    }
    GameObject& drop = world_.create("drop");
    drop.add<Transform>(canTransform_->x + (canWidth - dropWidth) * 0.5F, canY - dropHeight, dropWidth, dropHeight);
    drop.add<Motion>(0.0F, -dropSpeed);
    drop.add<Collider>(Collider::Kind::Trigger, sprayLayer);
    drop.add<Renderable>(dropColor).layer = 3;
    drop.setActive(true);
    return true;
}

bool Match::openUmbrella()
{
    if (umbrella_ > 0.0F || cooldown_ > 0.0F || !can_->isActive() || state_ != State::Playing) {
        return false;
    }
    umbrella_ = umbrellaSeconds;
    cooldown_ = umbrellaSeconds + umbrellaCooldown;
    return true;
}

int Match::pestsAlive() const
{
    return countActive("pest");
}

void Match::onCollision(const Event& event)
{
    if (event.origin != 0 || event.getOr<std::string>("phase", "") != "enter") {
        return;
    }
    GameObject* a = world_.find(static_cast<ObjectId>(event.getOr<std::int64_t>("a", 0)));
    GameObject* b = world_.find(static_cast<ObjectId>(event.getOr<std::int64_t>("b", 0)));
    if (a == nullptr || b == nullptr || !a->isActive() || !b->isActive()) {
        return;
    }
    GameObject* first = nullptr;
    GameObject* second = nullptr;
    const auto pair = [&](const char* firstTag, const char* secondTag) {
        if (a->tag() == firstTag && b->tag() == secondTag) {
            first = a;
            second = b;
        } else if (b->tag() == firstTag && a->tag() == secondTag) {
            first = b;
            second = a;
        } else {
            return false;
        }
        return true;
    };

    if (pair("drop", "pest") || pair("drop", "ladybug")) {
        remove(*first);
        // Out of play at once; the DeathEvent scores and removes it.
        second->setActive(false);
        events_.raise(Event(Events::Death).set("object", second->id()).set("cause", "sprayed"));
    } else if (pair("seed", "can")) {
        remove(*first);
        if (umbrella_ > 0.0F || shield_ > 0.0F) {
            ++blocked_;
            return;
        }
        can_->setActive(false);
        Event death(Events::Death);
        death.set("object", can_->id()).set("cause", "seed");
        death.priority = EventPriority::High;
        events_.raise(std::move(death));
    } else if (pair("pest", "fence") && state_ == State::Playing) {
        events_.raise(Event(Events::Death).set("object", can_->id()).set("cause", "overrun"));
    }
}

void Match::onDeath(const Event& event)
{
    if (event.origin != 0) {
        return;
    }
    GameObject* object = world_.find(static_cast<ObjectId>(event.getOr<std::int64_t>("object", 0)));
    if (object == nullptr) {
        return;
    }
    const std::string cause = event.getOr<std::string>("cause", "");

    if (cause == "sprayed") {
        const Pest* pest = object->get<Pest>();
        score_ += pest != nullptr ? pointsForRow(pest->row) : ladybugPoints;
        scatter(*object->get<Transform>(), pest != nullptr ? pestColor(pest->row) : ladybugColor, splatsPerPest);
        remove(*object);
        // Several pests can fall in one frame; only the first to empty the
        // formation clears the wave.
        if (pest != nullptr && state_ == State::Playing && pestsAlive() == 0) {
            for (const auto& other : world_.objects()) {
                if (other->isActive() && other->tag() == "seed") {
                    remove(*other);
                }
            }
            bloom();
            if (wave_ == 1) {
                state_ = State::WaveCleared;
                events_.raise(Event(Events::Spawn)
                                  .set("kind", "wave")
                                  .set("wave", 2)
                                  .set("generation", static_cast<std::int64_t>(generation_)),
                              ticsFor(waveBreakSeconds));
            } else {
                state_ = State::Won;
            }
        }
    } else if (cause == "seed" && object == can_) {
        if (state_ == State::Lost || state_ == State::Won) {
            return;
        }
        can_->setActive(false);
        --lives_;
        scatter(*canTransform_, dropColor, splatsPerPest);
        if (lives_ <= 0) {
            state_ = State::Lost;
            return;
        }
        Event spawn(Events::Spawn);
        spawn.set("kind", "can").set("generation", static_cast<std::int64_t>(generation_));
        spawn.priority = EventPriority::High;
        events_.raise(std::move(spawn), ticsFor(respawnSeconds));
    } else if (cause == "overrun") {
        if (state_ == State::Lost || state_ == State::Won) {
            return;
        }
        state_ = State::Lost;
        lives_ = 0;
        can_->setActive(false);
    }
}

void Match::onSpawn(const Event& event)
{
    if (event.origin != 0 || event.getOr<std::int64_t>("generation", -1) != static_cast<std::int64_t>(generation_)) {
        return;
    }
    const std::string kind = event.getOr<std::string>("kind", "");
    if (kind == "can") {
        if (state_ == State::Lost || state_ == State::Won) {
            return;
        }
        canTransform_->x = (width - canWidth) * 0.5F;
        canTransform_->y = canY;
        can_->setActive(true);
        shield_ = respawnShieldSeconds;
    } else if (kind == "wave") {
        wave_ = static_cast<int>(event.getOr<std::int64_t>("wave", 1));
        buildWave(wave_);
        state_ = State::Playing;
    }
}

void Match::buildWave(int wave)
{
    direction_ = 1.0F;
    marchClock_ = 0.0F;
    const float top = formationTop + (wave >= 2 ? waveTwoDrop : 0.0F);
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            GameObject& pest = world_.create("pest");
            pest.add<Transform>(formationLeft + column * columnSpacing, top + row * rowSpacing, pestWidth, pestHeight);
            pest.add<Pest>(row, column);
            pest.add<Collider>(Collider::Kind::Trigger, sprayLayer | gardenLayer);
            pest.setActive(true);
        }
    }
    // A moment's grace before the first seed.
    seedTimer_ = seedInterval(wave, roll()) + 1.0F;
}

void Match::march(float dt)
{
    std::vector<Transform*> pests;
    float left = std::numeric_limits<float>::max();
    float right = std::numeric_limits<float>::lowest();
    for (const auto& object : world_.objects()) {
        if (object->isActive() && object->tag() == "pest") {
            Transform* transform = object->get<Transform>();
            left = std::min(left, transform->x);
            right = std::max(right, transform->x + transform->width);
            pests.push_back(transform);
        }
    }
    if (pests.empty()) {
        return;
    }
    const March step = marchStep(left, right, direction_,
                                 marchSpeed(static_cast<int>(pests.size()), rows * columns, wave_), dt);
    direction_ = step.direction;
    for (Transform* transform : pests) {
        transform->x += step.dx;
        transform->y += step.dy;
    }
    marchClock_ += dt;
    if (marchClock_ >= marchFrameSeconds) {
        marchClock_ = std::fmod(marchClock_, marchFrameSeconds);
        marchFrame_ = !marchFrame_;
    }
}

void Match::dropSeed(float dt)
{
    seedTimer_ -= dt;
    if (seedTimer_ > 0.0F) {
        return;
    }
    seedTimer_ = seedInterval(wave_, roll());
    if (countActive("seed") >= maxSeeds) {
        return;
    }
    // Only the lowest pest of a column can drop a seed past its neighbours.
    std::array<const Transform*, columns> lowest{};
    for (const auto& object : world_.objects()) {
        if (!object->isActive() || object->tag() != "pest") {
            continue;
        }
        const Transform* transform = object->get<Transform>();
        const Transform*& slot = lowest[static_cast<std::size_t>(object->get<Pest>()->column)];
        if (slot == nullptr || transform->y > slot->y) {
            slot = transform;
        }
    }
    std::vector<const Transform*> droppers;
    for (const Transform* transform : lowest) {
        if (transform != nullptr) {
            droppers.push_back(transform);
        }
    }
    if (droppers.empty()) {
        return;
    }
    const Transform& from = *droppers[std::uniform_int_distribution<std::size_t>(0, droppers.size() - 1)(rng_)];
    GameObject& seed = world_.create("seed");
    seed.add<Transform>(from.x + (pestWidth - seedWidth) * 0.5F, from.y + pestHeight, seedWidth, seedHeight);
    seed.add<Motion>(0.0F, seedSpeed);
    seed.add<Collider>(Collider::Kind::Trigger, seedLayer);
    seed.add<Renderable>(seedColor).layer = 3;
    seed.setActive(true);
}

void Match::sendLadybug(float dt)
{
    if (countActive("ladybug") > 0) {
        return;
    }
    ladybugTimer_ -= dt;
    if (ladybugTimer_ > 0.0F) {
        return;
    }
    ladybugTimer_ = ladybugIntervalMin + (ladybugIntervalMax - ladybugIntervalMin) * roll();
    const float offLeft = -ladybugWidth - 10.0F;
    const float offRight = width + 10.0F;
    const float start = ladybugLeftward_ ? offRight : offLeft;
    ladybugEndX_ = ladybugLeftward_ ? offLeft : offRight;
    ladybugLeftward_ = !ladybugLeftward_;

    GameObject& ladybug = world_.create("ladybug");
    ladybug.add<Transform>(start, ladybugY, ladybugWidth, ladybugHeight);
    ladybug.add<PathMover>(PathMover::Linear{start, ladybugY, ladybugEndX_, ladybugY}, ladybugSpeed);
    ladybug.add<Collider>(Collider::Kind::Trigger, sprayLayer);
    ladybug.setActive(true);
}

void Match::cullStray()
{
    for (const auto& object : world_.objects()) {
        if (!object->isActive()) {
            continue;
        }
        const Transform& transform = *object->get<Transform>();
        const std::string& tag = object->tag();
        // PathMover stops exactly on the end of its path before turning back.
        if ((tag == "drop" && transform.y + transform.height < 0.0F) ||
            (tag == "seed" && transform.y > static_cast<float>(height)) ||
            (tag == "ladybug" && std::abs(transform.x - ladybugEndX_) < 0.5F)) {
            remove(*object);
        }
    }
}

void Match::scatter(const Transform& from, Color color, int count)
{
    const float centerX = from.x + from.width * 0.5F;
    const float centerY = from.y + from.height * 0.5F;
    for (int i = 0; i < count; ++i) {
        const float angle = twoPi * roll();
        const float speed = 60.0F + 120.0F * roll();
        if (!spawnBit(centerX, centerY, std::cos(angle) * speed, std::sin(angle) * speed - 80.0F, color,
                      splatSeconds * (0.6F + 0.4F * roll()))) {
            skipped_ += static_cast<std::uint64_t>(count - i - 1);
            return;
        }
    }
}

// Petals burst over the garden when a wave is cleared. More than the
// cosmetic budget on purpose, so the pool counter shows the skipping.
void Match::bloom()
{
    for (int i = 0; i < bloomPetals; ++i) {
        const float angle = twoPi * roll();
        const float speed = 80.0F + 220.0F * roll();
        const Color color = petalColors[static_cast<std::size_t>(i) % petalColors.size()];
        if (!spawnBit(width * 0.5F, height * 0.45F, std::cos(angle) * speed, std::sin(angle) * speed - 120.0F, color,
                      bloomSeconds * (0.6F + 0.4F * roll()))) {
            skipped_ += static_cast<std::uint64_t>(bloomPetals - i - 1);
            return;
        }
    }
}

bool Match::spawnBit(float x, float y, float velocityX, float velocityY, Color color, float seconds)
{
    GameObject* bit = world_.objects().size() + gameplayReserve < objectCapacity ? world_.tryCreate("splat") : nullptr;
    if (bit == nullptr) {
        ++skipped_;
        return false;
    }
    bit->add<Transform>(x - 2.0F, y - 2.0F, 4.0F, 4.0F);
    bit->add<Motion>(velocityX, velocityY);
    bit->add<Gravity>(420.0F);
    bit->add<Renderable>(color).layer = 4;
    bit->add<Lifetime>(world_, seconds);
    bit->setActive(true);
    return true;
}

void Match::remove(GameObject& object)
{
    object.setActive(false);
    world_.destroy(object.id());
}

int Match::countActive(const char* tag) const
{
    int count = 0;
    for (const auto& object : world_.objects()) {
        if (object->isActive() && object->tag() == tag) {
            ++count;
        }
    }
    return count;
}

} // namespace GardenDefense
