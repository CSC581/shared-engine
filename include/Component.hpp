#pragma once

#include "FrameTime.hpp"

class GameObject;

// Where each kind of component runs within World::updateComponents(). Lower
// runs first. Movement-style components are ordered so that every step reads
// what the previous one wrote this frame: game logic sets intent, a path or
// gravity turns it into velocity, and Motion turns velocity into position.
// Collision runs after all of them, as its own World pass.
namespace UpdateOrder {
constexpr int Behavior = 0;
constexpr int Path = 10;
constexpr int Gravity = 20;
constexpr int Motion = 30;
constexpr int Default = 40;
} // namespace UpdateOrder

// The generic component interface. A component holds its own attributes and
// provides behaviour by overriding the hooks below; anything a component does
// not need, it leaves alone.
//
// A component belongs to exactly one GameObject, which creates and destroys it
// (composition). It reaches its siblings through owner(), normally once, in
// onAttach(), caching the pointer it needs:
//
//     void onAttach() override { transform_ = &owner().require<Transform>(); }
//
// Components keep raw back-pointers, so they are neither copyable nor movable.
class Component {
public:
    Component() = default;
    virtual ~Component() = default;

    Component(const Component&) = delete;
    Component& operator=(const Component&) = delete;
    Component(Component&&) = delete;
    Component& operator=(Component&&) = delete;

    // Called once, right after the component is attached and owner() is valid.
    virtual void onAttach() {}

    // Called once per frame by World, in priority() order across all objects.
    virtual void update(const FrameTime& time) { (void)time; }

    // Read when World builds its update order, which it keeps until an object
    // or component is added or removed, so it must not change after attach.
    virtual int priority() const { return UpdateOrder::Default; }

    GameObject& owner() const { return *owner_; }

private:
    friend class GameObject;

    GameObject* owner_ = nullptr;
};
