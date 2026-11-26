#pragma once

#include <cstdint>
#include <vector>

namespace Frostbrew {

class Terrain {
public:
    Terrain(int width, int height, float surface);
    void generate(std::uint32_t seed);
    void edit(float x, float y, float radiusX, float radiusY, bool growth);
    float support(float left, float width) const;
    bool touches(float x, float y, float radius) const;
    const std::vector<float>& heights() const;
    int width() const;
    int height() const;

private:
    int height_;
    std::vector<float> heights_;
};

} // namespace Frostbrew
