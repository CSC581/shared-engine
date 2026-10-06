#pragma once

#include "Component.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using ObjectId = std::uint32_t;

namespace detail {

inline std::size_t nextComponentTypeId()
{
    static std::atomic<std::size_t> next{0};
    return next++;
}

} // namespace detail

// A small number unique to each component type, handed out on first use
// (Transform might be 0, Motion 1, ...). GameObject keeps its components in a
// slot per number, so get<T>() is one array read instead of a search.
template <class T>
std::size_t componentTypeId()
{
    static const std::size_t id = detail::nextComponentTypeId();
    return id;
}

// A game object is an id, a tag and the components it owns, and nothing else.
// Position, velocity, collision and drawing are all components, so a new kind
// of object is a new combination of them rather than a new class.
//
// Objects are created by World, which owns them. Components hold pointers back
// to their object, so a GameObject never moves once created.
//
// A new object starts inactive, so World never updates, collides or draws it
// half-built. Attach its components, then call setActive(true). Games wrap this
// in one factory function per kind of object (makePlayer, makePlatform), which
// plays the role a constructor would in a class-per-type design:
//
//     GameObject& makeCrate(World& world, float x, float y)
//     {
//         GameObject& crate = world.create("crate");
//         crate.add<Transform>(x, y, 32.0F, 32.0F);
//         crate.add<Gravity>();
//         crate.add<Collider>();
//         crate.setActive(true);
//         return crate;
//     }
//
// At most one component of each type is attached. Lookups match the exact type
// a component was added as: get<Collider>() does not find a subclass of
// Collider added as itself, so write a new component rather than subclassing a
// built-in one. Attaching happens on the thread that owns the World.
class GameObject {
public:
    GameObject(ObjectId id, std::string tag);

    GameObject(const GameObject&) = delete;
    GameObject& operator=(const GameObject&) = delete;
    GameObject(GameObject&&) = delete;
    GameObject& operator=(GameObject&&) = delete;

    ObjectId id() const;

    // A free-form label used for lookups ("player", "platform", "spawn").
    const std::string& tag() const;
    void setTag(std::string tag);

    // An inactive object is skipped by every World pass and by rendering. New
    // objects start inactive; see above.
    bool isActive() const;
    void setActive(bool active);

    // Creates a T, attaches it and calls its onAttach(). Throws if a T is
    // already attached.
    template <class T, class... Args>
    T& add(Args&&... args)
    {
        static_assert(std::is_base_of_v<Component, T>, "T must derive from Component");

        if (get<T>() != nullptr) {
            throw std::logic_error("GameObject already has a component of this type");
        }

        auto component = std::make_unique<T>(std::forward<Args>(args)...);
        T& attached = *component;
        attached.owner_ = this;
        components_.push_back(std::move(component));

        // Filled before onAttach(), which may add more components and grow
        // the slots.
        const std::size_t id = componentTypeId<T>();
        if (id >= slots_.size()) {
            slots_.resize(id + 1, nullptr);
        }
        slots_[id] = &attached;
        if (structureVersion_ != nullptr) {
            ++*structureVersion_;
        }

        attached.onAttach();
        return attached;
    }

    // The attached T, or nullptr.
    template <class T>
    T* get()
    {
        return static_cast<T*>(slot(componentTypeId<T>()));
    }

    template <class T>
    const T* get() const
    {
        return static_cast<const T*>(slot(componentTypeId<T>()));
    }

    template <class T>
    bool has() const
    {
        return get<T>() != nullptr;
    }

    // The attached T, adding a default-constructed one first if there is none.
    // This is how a component declares what it depends on.
    template <class T>
    T& require()
    {
        if (T* existing = get<T>()) {
            return *existing;
        }
        return add<T>();
    }

    const std::vector<std::unique_ptr<Component>>& components() const;

private:
    friend class World;

    Component* slot(std::size_t id) const
    {
        return id < slots_.size() ? slots_[id] : nullptr;
    }

    ObjectId id_;
    std::string tag_;
    bool active_ = false;

    // Owns the components, in attach order.
    std::vector<std::unique_ptr<Component>> components_;

    // The same components indexed by componentTypeId(); nullptr where the
    // object has no component of that type.
    std::vector<Component*> slots_;

    // The owning World's structure counter, bumped on every add() so the World
    // knows its cached update order is out of date. Set by World::create();
    // null for an object made outside a World.
    std::uint64_t* structureVersion_ = nullptr;
};
