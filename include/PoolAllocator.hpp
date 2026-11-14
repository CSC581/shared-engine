#pragma once

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

template <class T>
class PoolAllocator;

namespace detail {

class PoolStorage {
    template <class T>
    friend class ::PoolAllocator;

    PoolStorage(std::size_t slotSize, std::size_t alignment, std::size_t capacity,
                void (*destroy)(void*) noexcept);
    ~PoolStorage() noexcept;

    PoolStorage(const PoolStorage&) = delete;
    PoolStorage& operator=(const PoolStorage&) = delete;

    static std::size_t checkedCapacity(std::size_t slotSize, std::size_t capacity);
    std::size_t reserve() noexcept;
    void* address(std::size_t index) const noexcept;
    void release(std::size_t index) noexcept;
    void destroy(void* object);
    std::size_t size() const;
    std::size_t capacity() const;

    std::vector<std::size_t> free_;
    std::vector<unsigned char> live_;
    std::size_t slotSize_;
    std::size_t alignment_;
    void (*destroy_)(void*) noexcept;
    void* memory_;
};

} // namespace detail

// Owns fixed, aligned T slots; operations belong to one owning thread.
// T's constructors/destructors must not re-enter this pool.
template <class T>
class PoolAllocator {
    static_assert(std::is_nothrow_destructible_v<T>, "Pooled objects need nonthrowing destructors");

public:
    explicit PoolAllocator(std::size_t capacity)
        : storage_(sizeof(T), alignof(T), capacity,
                   [](void* object) noexcept { std::launder(static_cast<T*>(object))->~T(); })
    {}

    ~PoolAllocator() noexcept = default;

    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;
    PoolAllocator(PoolAllocator&&) = delete;
    PoolAllocator& operator=(PoolAllocator&&) = delete;

    // Null means full; construction/allocation exceptions propagate unchanged.
    template <class... Args>
    T* create(Args&&... args)
    {
        const std::size_t index = storage_.reserve();
        if (index == capacity()) {
            return nullptr;
        }
        try {
            return ::new (storage_.address(index)) T(std::forward<Args>(args)...);
        } catch (...) {
            storage_.release(index);
            throw;
        }
    }

    // Reject invalid slots before dereferencing; stale pointers after reuse
    // need generation handles and cannot be identified by this pointer API.
    void destroy(T* object) { storage_.destroy(object); }

    std::size_t size() const { return storage_.size(); }
    std::size_t capacity() const { return storage_.capacity(); }

private:
    detail::PoolStorage storage_;
};
