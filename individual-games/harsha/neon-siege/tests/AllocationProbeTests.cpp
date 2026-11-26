#include "AllocationProbe.hpp"
#include <cstdint>
#include <iostream>
#include <limits>
#include <new>

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; ++failures; } } while (false)
}
int main()
{
    void* before = ::operator new(11);
    AllocationProbe::start();
    ::operator delete(before);
    CHECK(AllocationProbe::counts().live == 0);
    void* a = ::operator new(23);
    void* b = ::operator new[](31);
    void* c = ::operator new(17, std::align_val_t(256));
    void* zero = ::operator new(0);
    CHECK(reinterpret_cast<std::uintptr_t>(c) % 256 == 0);
    auto count = AllocationProbe::counts();
    CHECK(count.requests == 4 && count.bytes == 71 && count.live == 71 && count.peak == 71);
    ::operator delete(a, std::size_t(23));
    ::operator delete[](b);
    ::operator delete(c, std::size_t(17), std::align_val_t(256));
    ::operator delete(zero);
    CHECK(AllocationProbe::counts().live == 0);
    AllocationProbe::resetPeak(); CHECK(AllocationProbe::counts().peak == 0);
    auto* fail = ::operator new(std::numeric_limits<std::size_t>::max(), std::nothrow);
    CHECK(fail == nullptr);
    CHECK(AllocationProbe::counts().requests == 4);
    void* d = ::operator new[](19, std::align_val_t(64), std::nothrow);
    CHECK(d != nullptr && reinterpret_cast<std::uintptr_t>(d) % 64 == 0);
    AllocationProbe::stop();
    ::operator delete[](d, std::align_val_t(64), std::nothrow);
    CHECK(AllocationProbe::counts().live == 0);
    AllocationProbe::start();
    CHECK(AllocationProbe::counts().requests == 0 && AllocationProbe::counts().bytes == 0);
    auto* scalar = ::operator new(9, std::nothrow);
    ::operator delete(scalar, std::nothrow);
    auto* array = ::operator new[](7, std::nothrow);
    ::operator delete[](array, std::size_t(7));
    CHECK(AllocationProbe::counts().requests == 2 && AllocationProbe::counts().bytes == 16);
    CHECK(AllocationProbe::counts().live == 0);
    AllocationProbe::stop();
    std::cout << "Allocation probe: " << failures << " failures\n";
    return failures != 0;
}
