#pragma once

namespace Net {

// The two client-server encodings supported by the engine. Selection is
// introduced at the transport boundary in Part 1C Phase 3; Phase 2 defines
// the value here so codecs and callers share one vocabulary.
enum class DataFormat {
    TextMultipart,
    Binary,
};

} // namespace Net
