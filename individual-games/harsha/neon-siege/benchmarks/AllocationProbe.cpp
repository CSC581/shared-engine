#include "AllocationProbe.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>

namespace {
struct Header { void* base; std::size_t requested; bool counted; };
std::atomic<bool> enabled{false};
std::atomic<std::uint64_t> requests{0}, bytes{0}, live{0}, peak{0};
void* allocate(std::size_t size, std::size_t alignment)
{
    alignment = std::max(alignment, alignof(std::max_align_t));
    const auto max = std::numeric_limits<std::size_t>::max();
    if ((alignment & (alignment - 1)) != 0 || alignment > max - sizeof(Header) ||
        size > max - sizeof(Header) - alignment) throw std::bad_alloc();
    void* base = nullptr;
    while (!(base = std::malloc(std::max<std::size_t>(size, 1) + sizeof(Header) + alignment))) {
        const auto handler = std::get_new_handler();
        if (!handler) throw std::bad_alloc();
        handler();
    }
    const auto address = (reinterpret_cast<std::uintptr_t>(base) + sizeof(Header) + alignment - 1) & ~(alignment - 1);
    auto* header = ::new (reinterpret_cast<void*>(address - sizeof(Header))) Header{base, size, enabled.load()};
    if (header->counted) {
        ++requests; bytes.fetch_add(size);
        const auto current = live.fetch_add(size) + size;
        auto old = peak.load();
        while (old < current && !peak.compare_exchange_weak(old, current)) {}
    }
    return reinterpret_cast<void*>(address);
}
void release(void* pointer) noexcept
{
    if (!pointer) return;
    auto* header = reinterpret_cast<Header*>(reinterpret_cast<std::uintptr_t>(pointer) - sizeof(Header));
    if (header->counted) live.fetch_sub(header->requested);
    std::free(header->base);
}
}
namespace AllocationProbe {
void start()
{
    if (enabled.load() || live.load() != 0) throw std::logic_error("Allocation probe is already active or owns live requests");
    requests = 0; bytes = 0; peak = 0; enabled = true;
}
void stop() { enabled = false; }
Counts counts() { return {requests.load(), bytes.load(), live.load(), peak.load()}; }
void resetPeak() { peak = live.load(); }
}
void* operator new(std::size_t size) { return allocate(size, alignof(std::max_align_t)); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, std::align_val_t alignment) { return allocate(size, static_cast<std::size_t>(alignment)); }
void* operator new[](std::size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, std::size_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t) noexcept { release(p); }
void operator delete(void* p, std::align_val_t) noexcept { release(p); }
void operator delete[](void* p, std::align_val_t) noexcept { release(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { release(p); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { try { return ::operator new(n); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { try { return ::operator new[](n); } catch (...) { return nullptr; } }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { try { return ::operator new(n, a); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { try { return ::operator new[](n, a); } catch (...) { return nullptr; } }
void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { release(p); }
