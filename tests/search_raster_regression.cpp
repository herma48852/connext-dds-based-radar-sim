#include "BeamPatternModel.hpp"
#include "RadarFaces.hpp"
#include "SearchRaster.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <set>
#include <tuple>

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
    namespace faces = radar::faces;
    namespace raster = radar::app::search_raster;

    std::array<raster::FaceRasterState, faces::kFaceCount> states{};
    std::set<std::tuple<int, int, int>> unique_pointings;
    std::array<
        std::array<int, raster::kElevationBarsDeg.size()>,
        faces::kFaceCount> bar_counts{};

    for (int dwell = 0; dwell < raster::kPointingsPerFace; ++dwell) {
        for (const auto& face : faces::kDefinitions) {
            const auto pointing =
                raster::advance_face(
                    face.id,
                    states[static_cast<std::size_t>(face.id)]);
            const int azimuth_index = static_cast<int>(std::lround(
                (pointing.azimuth_deg - face.coverage_start_deg
                 - 0.5 * raster::kAzimuthStepDeg)
                / raster::kAzimuthStepDeg));
            int elevation_bar = -1;
            for (std::size_t bar = 0;
                 bar < raster::kElevationBarsDeg.size(); ++bar) {
                if (std::fabs(
                        pointing.elevation_deg
                        - raster::kElevationBarsDeg[bar]) < 1.0e-12) {
                    elevation_bar = static_cast<int>(bar);
                }
            }
            unique_pointings.emplace(
                face.id, azimuth_index, elevation_bar);
            ++bar_counts[static_cast<std::size_t>(face.id)]
                        [static_cast<std::size_t>(elevation_bar)];
        }
    }

    check(unique_pointings.size()
              == static_cast<std::size_t>(raster::kPointingsAllFaces),
          "four-face raster contains 480 unique keyed pointings");
    for (const auto& face_counts : bar_counts) {
        for (const int count : face_counts) {
            check(count == raster::kAzimuthDwellsPerFace,
                  "each face/bar contains exactly forty azimuth centers");
        }
    }
    for (const auto& state : states) {
        check(state.azimuth_index == 0 && state.elevation_bar == 0,
              "120 dwells return every face raster to its initial state");
    }
    check(std::fabs(raster::kFaceVolumePeriodSec - 1.2) < 1.0e-12,
          "concurrent faces complete 480 keyed pointings in 1.2 seconds");
    check(std::fabs(raster::kAggregateCommandRateHz - 400.0) < 1.0e-12,
          "four 100 Hz faces publish 400 BeamCommands per second");

    for (const auto& face : faces::kDefinitions) {
        raster::FaceRasterState state;
        const auto first = raster::advance_face(face.id, state);
        check(std::fabs(
                  first.azimuth_deg - face.coverage_start_deg
                  - 0.5 * raster::kAzimuthStepDeg) < 1.0e-12,
              "first beam center is half a spacing inside its face edge");
        raster::Pointing last = first;
        for (int i = 1; i < raster::kAzimuthDwellsPerFace; ++i)
            last = raster::advance_face(face.id, state);
        check(std::fabs(
                  last.azimuth_deg - face.coverage_end_deg
                  + 0.5 * raster::kAzimuthStepDeg) < 1.0e-12 ||
              (face.id == faces::kForwardPort &&
               std::fabs(last.azimuth_deg - 358.875) < 1.0e-12),
              "last beam center is half a spacing inside its face edge");
    }

    raster::SectorRasterState sector;
    std::array<double, raster::kSectorAzimuthDwells> sector_centers{};
    for (auto& center : sector_centers)
        center = raster::advance_sector(45.0, sector).azimuth_deg;
    check(std::fabs(sector_centers.front() - 31.5) < 1.0e-12 &&
              std::fabs(sector_centers.back() - 58.5) < 1.0e-12,
          "30-degree sector uses centers from -13.5 to +13.5 degrees");
    for (std::size_t i = 1; i < sector_centers.size(); ++i) {
        check(std::fabs(
                  sector_centers[i] - sector_centers[i - 1]
                  - raster::kAzimuthStepDeg) < 1.0e-12,
              "sector centers retain 2.25-degree spacing");
    }
    const auto reverse = raster::advance_sector(45.0, sector);
    check(std::fabs(reverse.azimuth_deg - 56.25) < 1.0e-12 &&
              std::fabs(reverse.elevation_deg - 14.0) < 1.0e-12,
          "sector reverses without repeating the endpoint and advances bar");

    const auto nominal = radar::app::BeamPatternModel::calculate(0u);
    const double overlap_deg =
        nominal.beamwidth_3db_deg - raster::kAzimuthStepDeg;
    check(overlap_deg > 0.8 && overlap_deg < 1.1,
          "physical HPBW overlaps adjacent 2.25-degree pointings");

    const double crossover_offset = raster::kAzimuthStepDeg * 0.5;
    constexpr double kHalfPowerAmplitude = 0.7071067811865476;
    check(nominal.relative_amplitude(crossover_offset)
              > kHalfPowerAmplitude,
          "adjacent-beam midpoint is inside both half-power footprints");

    if (failures != 0) {
        std::fprintf(stderr, "search_raster_regression: %d failure(s)\n",
                     failures);
        return 1;
    }
    std::printf(
        "search_raster_regression: PASS "
        "(4 faces, 480 pointings/1.2 s, 30-degree sector, %.2f deg HPBW)\n",
        nominal.beamwidth_3db_deg);
    return 0;
}
