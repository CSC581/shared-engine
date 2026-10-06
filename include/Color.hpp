#pragma once

#include <cstdint>

// An RGBA colour. Shared by the engine's screen clear and by Renderable, so
// it lives on its own: the object model must stay free of SDL headers, and
// Engine.hpp includes them.
struct Color {
    std::uint8_t red;
    std::uint8_t green;
    std::uint8_t blue;
    std::uint8_t alpha = 255;
};
