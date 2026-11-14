#include "PoolAllocator.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace detail {

PoolStorage::PoolStorage(std::size_t slotSize, std::size_t alignment, std::size_t capacity,
                         void (*destroy)(void*) noexcept)
    : free_(checkedCapacity(slotSize, capacity)), live_(capacity, 0),
      slotSize_(slotSize), alignment_(std::max(alignment, alignof(std::max_align_t))),
      destroy_(destroy), memory_(::operator new(slotSize * capacity, std::align_val_t(alignment_)))
{
    for (std::size_t i = 0; i < capacity; ++i) {
        free_[i] = capacity - i - 1;
    }
}

PoolStorage::~PoolStorage() noexcept
{
    for (std::size_t i = 0; i < capacity(); ++i) {
        if (live_[i]) {
            destroy_(address(i));
        }
    }
    ::operator delete(memory_, std::align_val_t(alignment_));
}

std::size_t PoolStorage::checkedCapacity(std::size_t slotSize, std::size_t capacity)
{
    if (capacity == 0) {
        throw std::invalid_argument("Pool capacity must be positive");
    }
    if (capacity > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / slotSize ||
        capacity > std::vector<std::size_t>{}.max_size() ||
        capacity > std::vector<unsigned char>{}.max_size()) {
        throw std::length_error("Pool capacity exceeds addressable storage");
    }
    return capacity;
}

std::size_t PoolStorage::reserve() noexcept
{
    if (free_.empty()) {
        return capacity();
    }
    const auto index = free_.back();
    free_.pop_back();
    live_[index] = 1;
    return index;
}

void* PoolStorage::address(std::size_t index) const noexcept
{
    return static_cast<unsigned char*>(memory_) + index * slotSize_;
}

void PoolStorage::release(std::size_t index) noexcept
{
    live_[index] = 0;
    free_.push_back(index);
}

void PoolStorage::destroy(void* object)
{
    const auto pointer = reinterpret_cast<std::uintptr_t>(object);
    const auto base = reinterpret_cast<std::uintptr_t>(memory_);
    if (object == nullptr || pointer < base) {
        throw std::invalid_argument("Pointer does not name a live pool slot");
    }
    const auto offset = pointer - base;
    const auto index = offset / slotSize_;
    if (offset % slotSize_ != 0 || index >= capacity() || !live_[index]) {
        throw std::invalid_argument("Pointer does not name a live pool slot");
    }
    destroy_(object);
    release(index);
}

std::size_t PoolStorage::size() const
{
    return capacity() - free_.size();
}

std::size_t PoolStorage::capacity() const
{
    return live_.size();
}

} // namespace detail
