// Grid path queries on their own: Physics.PathRegions floods and FindPath
// searches over a 100 x 100 unit room grid (walls every 10 units with door
// gaps) at 0.25-unit cells, the scale of Village_Rim's quest guide. The
// occupancy is a cheap analytic function, so the timings show the search
// bookkeeping rather than collider queries. Prints medians and a digest of
// the results so runs before and after a change can be compared.
//   path_benchmark [WARMUP] [SAMPLES]
#include "physics/PhysicsQueries.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using PhysicsQueries::PathCell;

// Walls are 0.5 thick at multiples of 10 inside (0, 100); each wall segment
// between two crossings has a 2-unit door at its middle, except every third
// one, which is closed so that some rooms are cut off.
bool InWall(float v, float other) {
    const float m = std::fmod(v, 10.0f);
    if (v < 0.0f || v > 100.0f || !(m < 0.25f || m > 9.75f)) return false;
    const int segment = static_cast<int>(std::floor(other / 10.0f));
    const int line = static_cast<int>(std::lround(v / 10.0f));
    const float d = std::fmod(other, 10.0f);
    const bool door = (segment + line) % 3 != 0 && d > 4.0f && d < 6.0f;
    return !door;
}
bool Solid(float x, float y) {
    if (x < 0.0f || y < 0.0f || x > 100.0f || y > 100.0f) return true;
    return InWall(x, y) || InWall(y, x);
}
PathCell Occupancy(float x, float y) {
    if (Solid(x, y)) return PathCell::Solid;
    constexpr float r = 0.3f;
    if (Solid(x - r, y - r) || Solid(x + r, y - r) || Solid(x - r, y + r) || Solid(x + r, y + r))
        return PathCell::Clearance;
    return PathCell::Free;
}

double Median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}
} // namespace

int main(int argc, char **argv) {
    try {
        const int warmup = argc > 1 ? std::stoi(argv[1]) : 2;
        const int samples = argc > 2 ? std::stoi(argv[2]) : 9;
        if (warmup < 0 || samples < 1) throw std::runtime_error("usage: path_benchmark [WARMUP] [SAMPLES]");
        // Door sides and room centres, as the guide labels portals, doors and chests.
        std::vector<Vector2> points;
        for (int i = 0; i < 10; ++i)
            for (int j = 0; j < 10; j += 3) points.push_back(Vector2(i * 10.0f + 5.0f, j * 10.0f + 5.0f));
        for (int i = 1; i < 10; ++i) points.push_back(Vector2(i * 10.0f - 0.4f, 45.0f));
        std::vector<double> regions_ms, path_ms;
        std::uint64_t digest = 1469598103934665603ull;
        const auto mix = [&](std::uint64_t value) { digest = (digest ^ value) * 1099511628211ull; };
        for (int run = 0; run < warmup + samples; ++run) {
            auto start = Clock::now();
            const std::vector<int> labels = PhysicsQueries::PathRegions(points, 0.25f, 200000, 0.3f, Occupancy);
            const double regions = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            start = Clock::now();
            const std::vector<Vector2> path = PhysicsQueries::FindPath(Vector2(5, 5), Vector2(95, 95), 0.25f, 200000,
                                                                       0.3f, Occupancy);
            const double search = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            if (run < warmup) continue;
            regions_ms.push_back(regions);
            path_ms.push_back(search);
            if (run == warmup) {
                for (int label : labels) mix(static_cast<std::uint64_t>(label));
                mix(path.size());
                for (const Vector2 &p : path) mix(static_cast<std::uint64_t>(std::lround(p.x * 8 + p.y * 8192)));
            }
        }
        std::cout << "path_benchmark points=" << points.size() << " cell=0.25 samples=" << samples
                  << " regions_median_ms=" << Median(regions_ms) << " find_path_median_ms=" << Median(path_ms)
                  << " digest=" << std::hex << digest << "\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "path_benchmark: " << error.what() << "\n";
        return 1;
    }
}
