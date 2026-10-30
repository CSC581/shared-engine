#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

using EventValue = std::variant<bool, std::int64_t, double, std::string>;

namespace Events {
// Stable names shared by the engine and games. Death and Spawn are raised by
// game rules; this local engine slice produces Collision and Input.
constexpr const char* Collision = "CollisionEvent";
constexpr const char* Death = "DeathEvent";
constexpr const char* Spawn = "SpawnEvent";
constexpr const char* Input = "InputEvent";
} // namespace Events

namespace EventPriority {
constexpr int High = -100;
constexpr int Normal = 0;
constexpr int Low = 100;
} // namespace EventPriority

// Copyable data only: queued events must not own or borrow game objects.
// Object IDs can be stored as int64_t and resolved by a handler when delivered.
struct Event {
    explicit Event(std::string typeName = {}) : type(std::move(typeName)) {}

    std::string type;
    std::map<std::string, EventValue> args;
    std::int64_t timestamp = 0;
    std::int64_t deliverAt = 0;
    int priority = EventPriority::Normal;
    std::uint64_t sequence = 0;
    std::uint32_t age = 0; // Number of event-handler links in this chain.
    std::uint32_t origin = 0; // Local by default; reserved for a future network bridge.

    Event& set(std::string name, bool value)
    {
        args[std::move(name)] = value;
        return *this;
    }

    Event& set(std::string name, std::int64_t value)
    {
        args[std::move(name)] = value;
        return *this;
    }

    template <class T, std::enable_if_t<std::is_integral_v<T> &&
                                         !std::is_same_v<T, bool> &&
                                         !std::is_same_v<T, std::int64_t>, int> = 0>
    Event& set(std::string name, T value)
    {
        return set(std::move(name), static_cast<std::int64_t>(value));
    }

    Event& set(std::string name, double value)
    {
        args[std::move(name)] = value;
        return *this;
    }

    Event& set(std::string name, std::string value)
    {
        args[std::move(name)] = std::move(value);
        return *this;
    }

    Event& set(std::string name, const char* value)
    {
        return set(std::move(name), std::string(value));
    }

    template <class T>
    const T& get(const std::string& name) const
    {
        return std::get<T>(args.at(name));
    }

    template <class T>
    T getOr(const std::string& name, T fallback) const
    {
        const auto it = args.find(name);
        return it == args.end() ? std::move(fallback) : std::get<T>(it->second);
    }

    bool has(const std::string& name) const { return args.count(name) != 0; }
};
