#pragma once

#include "FrameTime.hpp"
#include "GameObject.hpp"
#include "PoolAllocator.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

class EventManager;

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
    struct ObjectDeleter {
        PoolAllocator<GameObject>* pool = nullptr;
        void operator()(GameObject* object) const noexcept;
    };
    using ObjectPtr = std::unique_ptr<GameObject, ObjectDeleter>;
    using ObjectList = std::vector<ObjectPtr>;

    // Existing worlds use the heap; a positive capacity opts into fixed slots.
    World() = default;
    explicit World(std::size_t objectCapacity);

    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // Creates an empty, inactive object. Attach its components, then call
    // setActive(true); until then no pass touches it. The reference stays valid
    // until the object is destroyed and flushDestroyed() runs. A full pool
    // throws std::bad_alloc; inactive objects still occupy their slots.
    GameObject& create(std::string tag = {});

    // Null only when the pool is full; other exceptions propagate.
    GameObject* tryCreate(std::string tag = {});

    // Zero means heap allocation; otherwise the fixed GameObject slot count.
    std::size_t objectCapacity() const;

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
    const ObjectList& objects() const;

    void update(const FrameTime& time);

    // An empty tag runs every active object; otherwise only objects with that
    // tag. Calls on disjoint tags may run on different threads at once.
    void updateComponents(const FrameTime& time, const std::string& tag = {});

    void resolveCollisions();

    // Optional event sink. Must outlive this World or be cleared before it
    // is destroyed. CollisionEvent args are a/b (local object IDs, smaller
    // first), tagA/tagB, phase ("enter", "stay" or "exit") and trigger (bool).
    // "stay" needs Collider::raiseStayEvents. The owner dispatches later.
    void setEventManager(EventManager* events);

    void flushDestroyed();

private:
    // Declared before objects_ so every owner releases its slot before the pool.
    std::unique_ptr<PoolAllocator<GameObject>> objectPool_;
    EventManager* events_ = nullptr;
    ObjectList objects_;
    // Every component of every object, sorted into update order. Rebuilt only
    // when structureVersion_ has moved past orderVersion_, that is when an
    // object was created or destroyed or a component attached, instead of
    // being re-sorted every frame. Guarded by orderMutex_ because
    // updateComponents() may run on several FrameWorkers at once.
    std::mutex orderMutex_;
    std::vector<Component*> order_;
    std::uint64_t structureVersion_ = 1;
    std::uint64_t orderVersion_ = 0;

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
