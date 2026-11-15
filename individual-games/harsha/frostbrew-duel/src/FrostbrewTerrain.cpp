#include "FrostbrewTerrain.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>

namespace Frostbrew {
Terrain::Terrain(int width, int height, float surface) : height_(height)
{
    if (width < 1 || width > 16384 || height < 96 || height > 16384 ||
        !std::isfinite(surface) || surface < std::max(64, height / 4) || surface > height - 16) {
        throw std::invalid_argument("Invalid terrain dimensions or surface");
    }
    heights_.assign(static_cast<std::size_t>(width), surface);
}
void Terrain::generate(std::uint32_t seed)
{
    std::mt19937 random(seed);
    double previous = static_cast<double>(random() % 101) / 100.0;
    double next = static_cast<double>(random() % 101) / 100.0;
    for (int x = 0; x < width(); ++x) {
        if (x != 0 && x % 160 == 0) {
            previous = next;
            next = static_cast<double>(random() % 101) / 100.0;
        }
        double t = (x % 160) / 160.0;
        t = t * t * (3 - 2 * t);
        const auto y = height_ * (0.60 + 0.16 * (previous + (next - previous) * t));
        heights_[x] = static_cast<float>(std::clamp(y, double(std::max(64, height_ / 4)), double(height_ - 16)));
    }
}
void Terrain::edit(float x, float y, float radiusX, float radiusY, bool growth)
{
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(radiusX) ||
        !std::isfinite(radiusY) || radiusX < 0 || radiusY < 0) {
        throw std::invalid_argument("Invalid terrain edit");
    }
    if (radiusX == 0 || radiusY == 0) return;
    const double left = std::max(0.0, double(x) - radiusX);
    const double right = std::min(double(width() - 1), double(x) + radiusX);
    if (left > right) return;
    for (int column = static_cast<int>(std::ceil(left)); column <= static_cast<int>(right); ++column) {
        const double offset = (double(column) - x) / radiusX;
        const double extent = radiusY * std::sqrt(std::max(0.0, 1 - offset * offset));
        const double upper = double(y) - extent, lower = double(y) + extent;
        const double old = heights_[column];
        // Only exposed, connected ground can change in a height map.
        if (old < upper || old > lower) continue;
        heights_[column] = static_cast<float>(std::clamp(growth ? upper : lower,
                                                        double(std::max(64, height_ / 4)), double(height_ - 16)));
    }
}
float Terrain::support(float left, float span) const
{
    if (!std::isfinite(left) || !std::isfinite(span) || span <= 0) {
        throw std::invalid_argument("Invalid terrain footprint");
    }
    const int first = static_cast<int>(std::clamp(std::floor(double(left)), 0.0, double(width() - 1)));
    const int last = static_cast<int>(std::clamp(std::ceil(double(left) + span) - 1, 0.0, double(width() - 1)));
    return *std::min_element(heights_.begin() + first, heights_.begin() + std::max(first, last) + 1);
}
bool Terrain::touches(float x, float y, float radius) const
{
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(radius) || radius < 0) {
        throw std::invalid_argument("Invalid terrain contact");
    }
    if (double(x) + radius < 0 || double(x) - radius >= width()) return false;
    return double(y) + radius >= support(x - radius, std::max(1.0F, radius * 2));
}
const std::vector<float>& Terrain::heights() const { return heights_; }
int Terrain::width() const { return static_cast<int>(heights_.size()); }
int Terrain::height() const { return height_; }
} // namespace Frostbrew
