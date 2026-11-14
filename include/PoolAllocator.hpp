#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

// Owns fixed, aligned T slots; operations belong to one owning thread.
// T's constructors/destructors must not re-enter this pool.
template <class T>
class PoolAllocator {
    static_assert(std::is_nothrow_destructible_v<T>, "Pooled objects need nonthrowing destructors");

public:
    explicit PoolAllocator(std::size_t capacity)
        : free_(checkedCapacity(capacity)), live_(capacity, 0),
          memory_(std::allocator<T>{}.allocate(capacity))
    {
        for (std::size_t i = 0; i < capacity; ++i) {
            free_[i] = capacity - i - 1;
        }
    }

    ~PoolAllocator() noexcept
    {
        for (std::size_t i = 0; i < capacity(); ++i) {
            if (live_[i]) {
                std::launder(memory_ + i)->~T();
            }
        }
        std::allocator<T>{}.deallocate(memory_, capacity());
    }

    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;
    PoolAllocator(PoolAllocator&&) = delete;
    PoolAllocator& operator=(PoolAllocator&&) = delete;

    // Null means full; construction/allocation exceptions propagate unchanged.
    template <class... Args>
    T* create(Args&&... args)
    {
        if (free_.empty()) {
            return nullptr;
        }
        const std::size_t index = free_.back();
        free_.pop_back();
        try {
            T* object = ::new (static_cast<void*>(memory_ + index)) T(std::forward<Args>(args)...);
            live_[index] = 1;
            return object;
        } catch (...) {
            free_.push_back(index);
            throw;
        }
    }

    // Reject invalid slots before dereferencing; stale pointers after reuse
    // need generation handles and cannot be identified by this pointer API.
    void destroy(T* object)
    {
        const auto address = reinterpret_cast<std::uintptr_t>(object);
        const auto base = reinterpret_cast<std::uintptr_t>(memory_);
        if (object == nullptr || address < base) {
            throw std::invalid_argument("Pointer does not name a live pool slot");
        }
        const auto offset = address - base;
        const auto index = offset / sizeof(T);
        if (offset % sizeof(T) != 0 || index >= capacity() || !live_[index]) {
            throw std::invalid_argument("Pointer does not name a live pool slot");
        }
        object->~T();
        live_[index] = 0;
        free_.push_back(index);
    }

    std::size_t size() const { return capacity() - free_.size(); }
    std::size_t capacity() const { return live_.size(); }

private:
    static std::size_t checkedCapacity(std::size_t capacity)
    {
        if (capacity == 0) {
            throw std::invalid_argument("Pool capacity must be positive");
        }
        if (capacity > std::allocator_traits<std::allocator<T>>::max_size(std::allocator<T>{}) ||
            capacity > std::vector<std::size_t>{}.max_size() ||
            capacity > std::vector<unsigned char>{}.max_size()) {
            throw std::length_error("Pool capacity exceeds addressable storage");
        }
        return capacity;
    }

    std::vector<std::size_t> free_;
    std::vector<unsigned char> live_;
    T* memory_;
};
