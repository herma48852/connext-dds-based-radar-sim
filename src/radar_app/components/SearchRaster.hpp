#pragma once

#include <array>
#include <cstddef>

#include "RadarFaces.hpp"

namespace radar::app::search_raster {

// Each face advances independently at 100 dwells/s. The scheduler publishes
// all four face-keyed commands every 10 ms, for an aggregate 400 commands/s.
inline constexpr double kDwellPeriodSec = 0.01;
inline constexpr double kAzimuthStepDeg = 2.25;
inline constexpr int kAzimuthDwellsPerFace = 40;
inline constexpr std::array<double, 3> kElevationBarsDeg{
    3.0, 14.0, 25.0};
inline constexpr int kPointingsPerFace =
    kAzimuthDwellsPerFace
    * static_cast<int>(kElevationBarsDeg.size());
inline constexpr int kPointingsAllFaces =
    kPointingsPerFace * faces::kFaceCount;
inline constexpr double kFaceVolumePeriodSec =
    kPointingsPerFace * kDwellPeriodSec;
inline constexpr double kAggregateCommandRateHz =
    faces::kFaceCount / kDwellPeriodSec;

// A 30-degree priority sector uses thirteen 2.25-degree centers from
// center-13.5 through center+13.5. The physical 3.1719-degree HPBW extends
// beyond the first/last center while retaining adjacent overlap.
inline constexpr double kSectorWidthDeg = 30.0;
inline constexpr int kSectorAzimuthDwells = 13;
inline constexpr double kSectorCenterHalfSpanDeg =
    (kSectorAzimuthDwells - 1) * kAzimuthStepDeg * 0.5;

static_assert(kAzimuthStepDeg * kAzimuthDwellsPerFace == 90.0);
static_assert(kPointingsPerFace == 120);
static_assert(kPointingsAllFaces == 480);
static_assert(kFaceVolumePeriodSec == 1.2);
static_assert(kAggregateCommandRateHz == 400.0);
static_assert(kSectorCenterHalfSpanDeg == 13.5);

struct Pointing {
    double azimuth_deg;
    double elevation_deg;
};

struct FaceRasterState {
    int azimuth_index = 0;
    int elevation_bar = 0;
};

struct SectorRasterState {
    int azimuth_index = 0;
    int direction = 1;
    int elevation_bar = 0;
};

inline Pointing advance_face(
        faces::FaceId face_id, FaceRasterState& state) noexcept {
    const auto* face = faces::find(face_id);
    if (face == nullptr)
        face = &faces::kDefinitions.front();

    const double azimuth_deg =
        face->coverage_start_deg
        + 0.5 * kAzimuthStepDeg
        + state.azimuth_index * kAzimuthStepDeg;
    const double elevation_deg =
        kElevationBarsDeg[static_cast<std::size_t>(state.elevation_bar)];

    ++state.azimuth_index;
    if (state.azimuth_index == kAzimuthDwellsPerFace) {
        state.azimuth_index = 0;
        state.elevation_bar =
            (state.elevation_bar + 1)
            % static_cast<int>(kElevationBarsDeg.size());
    }
    return Pointing{faces::wrap360(azimuth_deg), elevation_deg};
}

inline Pointing advance_sector(
        double center_deg, SectorRasterState& state) noexcept {
    const double offset_deg =
        -kSectorCenterHalfSpanDeg
        + state.azimuth_index * kAzimuthStepDeg;
    const Pointing result{
        faces::wrap360(center_deg + offset_deg),
        kElevationBarsDeg[static_cast<std::size_t>(state.elevation_bar)]};

    if (state.direction > 0) {
        if (state.azimuth_index + 1 == kSectorAzimuthDwells) {
            state.direction = -1;
            state.elevation_bar =
                (state.elevation_bar + 1)
                % static_cast<int>(kElevationBarsDeg.size());
            --state.azimuth_index;
        } else {
            ++state.azimuth_index;
        }
    } else {
        if (state.azimuth_index == 0) {
            state.direction = 1;
            state.elevation_bar =
                (state.elevation_bar + 1)
                % static_cast<int>(kElevationBarsDeg.size());
            ++state.azimuth_index;
        } else {
            --state.azimuth_index;
        }
    }
    return result;
}

} // namespace radar::app::search_raster
