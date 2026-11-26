#pragma once
#include <cstdint>

namespace AllocationProbe {
struct Counts { std::uint64_t requests, bytes, live, peak; };
// Single-threaded measurement boundaries; allocation hooks themselves use atomics.
void start();
void stop();
Counts counts();
void resetPeak();
}
