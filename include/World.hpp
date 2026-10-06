#pragma once

#include "FrameTime.hpp"
#include "GameObject.hpp"

#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Owns every GameObject in a scene and runs one frame of them.
//
// A frame is three passes, in this order:
//   1. updateComponents()  every component's update(), in priority order
//   2. resolveCollisions() Solid colliders pushed apart, collision callbacks called
//   3. flushDestroyed()    objects passed to destroy() are removed
// update() runs all three. A game that splits work across FrameWorkers calls
// updateComponents() once per worker with a different tag, then runs the other
// two passes on the main thread.
//
// Threads: creating objects and attaching components happen only on the thread
// that owns the World, never from a component's update() while updateComponents()
// runs on several FrameWorkers at once (another worker may be walking the object
// list). A game that spawns from an update() records what to spawn and creates
// it on the owning thread after the workers finish.
//
// destroy() is the exception: it may be called from any thread, including
// several update() calls running in parallel, and from collision callbacks.
// It only queues the id; the object is removed at flushDestroyed(), on the
// owning thread, once nothing is updating.
class World {
public:
    World() = default;

    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // Creates an empty, inactive object. Attach its components, then call
    // setActive(true); until then no pass touches it. The reference stays valid
    // until the object is destroyed and flushDestroyed() runs.
    GameObject& create(std::string tag = {});

    // Marks the object for removal at the next flushDestroyed(). Safe from any
    // thread; see above.
    void destroy(ObjectId id);

    GameObject* find(ObjectId id);
    const GameObject* find(ObjectId id) const;

    // The first object with this tag, or nullptr.
    GameObject* findByTag(const std::string& tag);

    std::vector<GameObject*> findAllByTag(const std::string& tag);

    template <class T>
    std::vector<GameObject*> findAllWith()
    {
        std::vector<GameObject*> result;
        for (const auto& object : objects_) {
            if (object->has<T>()) {
                result.push_back(object.get());
            }
        }
        return result;
    }

    // Every object, in creation order.
    const std::vector<std::unique_ptr<GameObject>>& objects() const;

    void update(const FrameTime& time);

    // An empty tag runs every active object; otherwise only objects with that
    // tag. Calls on disjoint tags may run on different threads at once.
    void updateComponents(const FrameTime& time, const std::string& tag = {});

    void resolveCollisions();

    void flushDestroyed();

private:
    std::vector<std::unique_ptr<GameObject>> objects_;
    // Filled by destroy() from any thread, drained by flushDestroyed().
    std::mutex pendingMutex_;
    std::vector<ObjectId> pendingDestroy_;
    ObjectId nextId_ = 1;

    // Pairs that overlapped in the last collision pass, smaller id first.
    // Compared with this pass's pairs to tell onEnter and onExit apart from
    // onCollide. Ids, not pointers, so a destroyed object leaves nothing
    // dangling.
    std::set<std::pair<ObjectId, ObjectId>> contacts_;
};
