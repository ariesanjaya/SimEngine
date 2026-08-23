#include "Sim/Reference/ImageCompare.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace sim::reference {
namespace {

Vec3 MeanOf(const Image& image, const Region& region) {
    if (region.width == 0 || region.height == 0) {
        return Vec3(0.0f);
    }
    Vec3 total(0.0f);
    std::size_t count = 0;
    const uint32_t x1 = std::min(region.x + region.width, image.width);
    const uint32_t y1 = std::min(region.y + region.height, image.height);
    for (uint32_t y = region.y; y < y1; ++y) {
        for (uint32_t x = region.x; x < x1; ++x) {
            total += image.At(x, y);
            ++count;
        }
    }
    return count == 0 ? Vec3(0.0f) : total / static_cast<float>(count);
}

Image Crop(const Image& image, const Region& region) {
    Image out;
    const uint32_t x1 = std::min(region.x + region.width, image.width);
    const uint32_t y1 = std::min(region.y + region.height, image.height);
    out.width = x1 > region.x ? x1 - region.x : 0;
    out.height = y1 > region.y ? y1 - region.y : 0;
    out.pixels.reserve(static_cast<std::size_t>(out.width) * out.height);
    for (uint32_t y = region.y; y < y1; ++y) {
        for (uint32_t x = region.x; x < x1; ++x) {
            out.pixels.push_back(image.At(x, y));
        }
    }
    return out;
}

}  // namespace

std::string ImageDifference::ToString() const {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(5);
    out << "rmse=" << rmse << " maks=" << maxAbsolute << " di (" << maxX << "," << maxY << ")"
        << " rerata " << meanA.x << "," << meanA.y << "," << meanA.z << " lawan " << meanB.x
        << "," << meanB.y << "," << meanB.z;
    return out.str();
}

ImageDifference Compare(const Image& a, const Image& b) {
    ImageDifference result;
    if (a.width != b.width || a.height != b.height || a.pixels.empty()) {
        return result;
    }

    double squared = 0.0;
    for (uint32_t y = 0; y < a.height; ++y) {
        for (uint32_t x = 0; x < a.width; ++x) {
            const Vec3 delta = a.At(x, y) - b.At(x, y);
            const float channels[3] = {std::abs(delta.x), std::abs(delta.y), std::abs(delta.z)};
            for (const float c : channels) {
                squared += static_cast<double>(c) * c;
                if (c > result.maxAbsolute) {
                    result.maxAbsolute = c;
                    result.maxX = x;
                    result.maxY = y;
                }
            }
        }
    }
    // Dibagi jumlah kanal, bukan jumlah piksel: RMSE yang menghitung tiga kanal
    // sebagai satu sampel menjawab angka yang berbeda dari yang sama namanya di
    // alat lain, dan angka yang tidak bisa dibandingkan lintas alat tidak
    // menyelesaikan apa pun.
    const double samples = static_cast<double>(a.pixels.size()) * 3.0;
    result.rmse = static_cast<float>(std::sqrt(squared / samples));
    result.meanA = a.Mean();
    result.meanB = b.Mean();
    return result;
}

std::vector<RegionDifference> CompareRegions(const Image& a, const Image& b,
                                             const std::vector<Region>& regions) {
    std::vector<RegionDifference> out;
    out.reserve(regions.size());
    for (const Region& region : regions) {
        out.push_back(RegionDifference{region.name, Compare(Crop(a, region), Crop(b, region))});
    }
    return out;
}

Vec3 RegionMean(const Image& image, const Region& region) { return MeanOf(image, region); }

}  // namespace sim::reference
