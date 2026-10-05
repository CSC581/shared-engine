#include "World.hpp"

#include "Collision.hpp"
#include "Components.hpp"

#include <algorithm>
#include <utility>

namespace {

struct Body {
    GameObject* object;
    Collider* collider;
    Transform* transform;
    // Null when the collision pass must not move this object.
    Motion* pushable;
};

// Only an object whose velocity is its own can be pushed. No Motion means it
// never moves; a kinematic Motion belongs to whatever drives it.
Motion* pushableMotion(GameObject& object)
{
    Motion* motion = object.get<Motion>();
    if (motion == nullptr || motion->kinematic) {
        return nullptr;
    }
    return motion;
}

void push(Body& body, float dx, float dy)
{
    body.transform->x += dx;
    body.transform->y += dy;
    if (dx != 0.0F) {
        body.pushable->velocityX = 0.0F;
    }
    if (dy != 0.0F) {
        body.pushable->velocityY = 0.0F;
    }
}

void separate(Body& a, Body& b)
{
    float dx = 0.0F;
    float dy = 0.0F;

    if (a.pushable != nullptr && b.pushable == nullptr) {
        if (Collision::getSeparation(a.transform->bounds(), b.transform->bounds(), dx, dy)) {
            push(a, dx, dy);
        }
    } else if (b.pushable != nullptr && a.pushable == nullptr) {
        if (Collision::getSeparation(b.transform->bounds(), a.transform->bounds(), dx, dy)) {
            push(b, dx, dy);
        }
    } else if (a.pushable != nullptr && b.pushable != nullptr) {
        // Two movable bodies share the push equally.
        if (Collision::getSeparation(a.transform->bounds(), b.transform->bounds(), dx, dy)) {
            push(a, dx * 0.5F, dy * 0.5F);
            push(b, -dx * 0.5F, -dy * 0.5F);
        }
    }
}

} // namespace

GameObject& World::create(std::string tag)
{
    objects_.push_back(std::make_unique<GameObject>(nextId_++, std::move(tag)));
    return *objects_.back();
}

void World::destroy(ObjectId id)
{
    pendingDestroy_.push_back(id);
}

GameObject* World::find(ObjectId id)
{
    for (const auto& object : objects_) {
        if (object->id() == id) {
            return object.get();
        }
    }
    return nullptr;
}

const GameObject* World::find(ObjectId id) const
{
    for (const auto& object : objects_) {
        if (object->id() == id) {
            return object.get();
        }
    }
    return nullptr;
}

GameObject* World::findByTag(const std::string& tag)
{
    for (const auto& object : objects_) {
        if (object->tag() == tag) {
            return object.get();
        }
    }
    return nullptr;
}

std::vector<GameObject*> World::findAllByTag(const std::string& tag)
{
    std::vector<GameObject*> result;
    for (const auto& object : objects_) {
        if (object->tag() == tag) {
            result.push_back(object.get());
        }
    }
    return result;
}

const std::vector<std::unique_ptr<GameObject>>& World::objects() const
{
    return objects_;
}

void World::update(const FrameTime& time)
{
    updateComponents(time);
    resolveCollisions();
    flushDestroyed();
}

void World::updateComponents(const FrameTime& time, const std::string& tag)
{
    // Collected first, so an object created by a component this frame waits
    // until the next one instead of invalidating the walk.
    std::vector<Component*> queue;
    for (const auto& object : objects_) {
        if (!object->isActive() || (!tag.empty() && object->tag() != tag)) {
            continue;
        }
        for (const auto& component : object->components()) {
            queue.push_back(component.get());
        }
    }

    // Stable, so equal priorities keep creation and attach order.
    std::stable_sort(queue.begin(), queue.end(), [](const Component* a, const Component* b) {
        return a->priority() < b->priority();
    });

    for (Component* component : queue) {
        component->update(time);
    }
}

void World::resolveCollisions()
{
    std::vector<Body> bodies;
    for (const auto& object : objects_) {
        if (!object->isActive()) {
            continue;
        }
        if (auto* collider = object->get<Collider>()) {
            bodies.push_back({ object.get(), collider, object->get<Transform>(), pushableMotion(*object) });
        }
    }

    // Callbacks run after every push, so each one sees settled positions and
    // may create or destroy objects without disturbing the pass.
    std::vector<std::pair<Body*, Body*>> hits;

    for (std::size_t i = 0; i < bodies.size(); ++i) {
        for (std::size_t j = i + 1; j < bodies.size(); ++j) {
            Body& a = bodies[i];
            Body& b = bodies[j];

            if ((a.collider->layers & b.collider->layers) == 0U ||
                !Collision::intersects(a.transform->bounds(), b.transform->bounds())) {
                continue;
            }

            if (a.collider->kind == Collider::Kind::Solid && b.collider->kind == Collider::Kind::Solid) {
                separate(a, b);
            }
            hits.emplace_back(&a, &b);
        }
    }

    for (auto& [a, b] : hits) {
        if (a->collider->onCollide) {
            a->collider->onCollide(*a->object, *b->object);
        }
        if (b->collider->onCollide) {
            b->collider->onCollide(*b->object, *a->object);
        }
    }
}

void World::flushDestroyed()
{
    if (pendingDestroy_.empty()) {
        return;
    }

    std::vector<ObjectId> doomed;
    doomed.swap(pendingDestroy_);

    objects_.erase(
        std::remove_if(objects_.begin(), objects_.end(),
            [&doomed](const std::unique_ptr<GameObject>& object) {
                return std::find(doomed.begin(), doomed.end(), object->id()) != doomed.end();
            }),
        objects_.end());
}
