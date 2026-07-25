#include "BeamPatternModel.hpp"
#include "SearchRaster.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <set>
#include <utility>

namespace {
int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
} // namespace

int main() {
    namespace raster = radar::app::search_raster;

    double azimuth_deg = 0.0;
    int elevation_bar = 0;
    std::set<std::pair<int, int>> unique_pointings;
    std::array<int, raster::kElevationBarsDeg.size()> bar_counts{};

    for (int dwell = 0; dwell < raster::kFullVolumeDwells; ++dwell) {
        const auto pointing =
            raster::advance(azimuth_deg, elevation_bar);
        const int azimuth_index = static_cast<int>(
            std::lround(pointing.azimuth_deg
                        / raster::kAzimuthStepDeg))
            % raster::kAzimuthDwellsPerRevolution;
        unique_pointings.emplace(azimuth_index, elevation_bar);
        ++bar_counts[static_cast<std::size_t>(elevation_bar)];
    }

    check(unique_pointings.size()
              == static_cast<std::size_t>(raster::kFullVolumeDwells),
          "full raster contains 480 unique azimuth/elevation pointings");
    for (const int count : bar_counts) {
        check(count == raster::kAzimuthDwellsPerRevolution,
              "each elevation bar contains exactly 160 azimuth dwells");
    }
    check(std::fabs(azimuth_deg) < 1.0e-12 && elevation_bar == 0,
          "480 dwells return the raster to its initial state");
    check(std::fabs(raster::kFullVolumePeriodSec - 4.8) < 1.0e-12,
          "100 Hz scheduler completes the 480-dwell raster in 4.8 seconds");

    const auto nominal = radar::app::BeamPatternModel::calculate(0u);
    const double overlap_deg =
        nominal.beamwidth_3db_deg - raster::kAzimuthStepDeg;
    check(overlap_deg > 0.8 && overlap_deg < 1.1,
          "S-band physical nominal HPBW overlaps adjacent 2.25-degree pointings");

    const double crossover_offset = raster::kAzimuthStepDeg * 0.5;
    constexpr double kHalfPowerAmplitude = 0.7071067811865476;
    check(nominal.relative_amplitude(crossover_offset)
              > kHalfPowerAmplitude,
          "midpoint between adjacent beams is inside both half-power footprints");
    check(std::fabs(
              nominal.relative_amplitude(crossover_offset)
              - nominal.relative_amplitude(-crossover_offset))
              < 1.0e-6,
          "nominal crossover response is symmetric");

    if (failures != 0) {
        std::fprintf(stderr, "search_raster_regression: %d failure(s)\n",
                     failures);
        return 1;
    }
    std::printf(
        "search_raster_regression: PASS (480 dwells, 4.8 s, %.2f deg HPBW)\n",
        nominal.beamwidth_3db_deg);
    return 0;
}
