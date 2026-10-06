#include "GameObject.hpp"

GameObject::GameObject(ObjectId id, std::string tag)
    : id_(id),
      tag_(std::move(tag))
{
}

ObjectId GameObject::id() const
{
    return id_;
}

const std::string& GameObject::tag() const
{
    return tag_;
}

void GameObject::setTag(std::string tag)
{
    tag_ = std::move(tag);
}

bool GameObject::isActive() const
{
    return active_;
}

void GameObject::setActive(bool active)
{
    active_ = active;
}

const std::vector<std::unique_ptr<Component>>& GameObject::components() const
{
    return components_;
}
