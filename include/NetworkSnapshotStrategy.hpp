#pragma once

namespace Net {

// How client-server world state is delivered. This is independent of the
// text/binary representation selected by DataFormat.
enum class SnapshotStrategy {
    FullSnapshot,
    PropertyDelta,
};

} // namespace Net
