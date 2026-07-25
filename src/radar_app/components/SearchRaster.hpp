#pragma once

#include <array>

namespace radar::app::search_raster {

inline constexpr double kDwellPeriodSec = 0.01; // 100 Hz
inline constexpr double kAzimuthStepDeg = 2.25;
inline constexpr int kAzimuthDwellsPerRevolution = 160;
inline constexpr std::array<double, 3> kElevationBarsDeg{
    3.0, 14.0, 25.0};
inline constexpr int kFullVolumeDwells =
    kAzimuthDwellsPerRevolution
    * static_cast<int>(kElevationBarsDeg.size());
inline constexpr double kFullVolumePeriodSec =
    kFullVolumeDwells * kDwellPeriodSec;

static_assert(
    kAzimuthStepDeg * kAzimuthDwellsPerRevolution == 360.0);
static_assert(kFullVolumeDwells == 480);
static_assert(kFullVolumePeriodSec == 4.8);

inline double wrap360(double angle_deg) noexcept {
    while (angle_deg >= 360.0) angle_deg -= 360.0;
    while (angle_deg < 0.0) angle_deg += 360.0;
    return angle_deg;
}

struct Pointing {
    double azimuth_deg;
    double elevation_deg;
};

// Advance one dwell in the repeating 160-azimuth x 3-elevation search
// raster. After exactly 480 calls, azimuth and elevation_bar return to their
// initial state.
inline Pointing advance(double& azimuth_deg, int& elevation_bar) noexcept {
    if (azimuth_deg + kAzimuthStepDeg >= 360.0)
        elevation_bar =
            (elevation_bar + 1)
            % static_cast<int>(kElevationBarsDeg.size());
    azimuth_deg = wrap360(azimuth_deg + kAzimuthStepDeg);
    return Pointing{
        azimuth_deg,
        kElevationBarsDeg[static_cast<std::size_t>(elevation_bar)]};
}

} // namespace radar::app::search_raster
