#pragma once
// Resolution-cell plot fusion before tracking.
//
// Adjacent azimuth beams overlap at their -3 dB footprints, and adjacent faces
// overlap at their 90-degree seams. One contact can therefore produce plots
// in two neighboring dwells. Reports that fall inside one modeled range/
// angle resolution cell are combined before tracking. SNR-derived power
// weighting places the resulting bearing between beam centers instead of
// forcing the tracker to jump by a complete 2.25-degree raster cell.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "DetectionIdentity.hpp"
#include "RadarFaces.hpp"
#include "RadarRfModel.hpp"

namespace radar::app {

struct FaceDetection {
    int32_t face_id;
    int64_t sim_millis;
    double range_m;
    double azimuth_deg;
    double elevation_deg;
    double snr_db;
    std::vector<DetectionIdentity> contributors;
};

inline double wrapped_azimuth_separation_deg(
        double lhs_deg, double rhs_deg) noexcept {
    double difference = faces::wrap360(lhs_deg) - faces::wrap360(rhs_deg);
    while (difference > 180.0) difference -= 360.0;
    while (difference < -180.0) difference += 360.0;
    return std::fabs(difference);
}

inline std::vector<FaceDetection> fuse_resolution_cell_detections(
        const std::vector<FaceDetection>& input) {
    constexpr int64_t kTimeToleranceMs = 30;
    constexpr double kRangeToleranceM =
        1.5 * rf_model::kRangeResolutionM;
    constexpr double kAzimuthToleranceDeg = 3.3;
    constexpr double kElevationToleranceDeg = 0.1;

    struct Cluster {
        FaceDetection representative;
        double weight_sum;
        double weighted_range;
        double weighted_elevation;
        double weighted_sin_azimuth;
        double weighted_cos_azimuth;
    };

    const auto weight_for = [](double snr_db) {
        return std::pow(
            10.0, std::clamp(snr_db, -40.0, 80.0) / 10.0);
    };
    const auto add_to_cluster = [&](Cluster& cluster,
                                    const FaceDetection& detection) {
        constexpr double kDeg2Rad =
            3.14159265358979323846 / 180.0;
        const double weight = weight_for(detection.snr_db);
        cluster.weight_sum += weight;
        cluster.weighted_range += weight * detection.range_m;
        cluster.weighted_elevation +=
            weight * detection.elevation_deg;
        cluster.weighted_sin_azimuth +=
            weight * std::sin(detection.azimuth_deg * kDeg2Rad);
        cluster.weighted_cos_azimuth +=
            weight * std::cos(detection.azimuth_deg * kDeg2Rad);

        auto& output = cluster.representative;
        output.range_m =
            cluster.weighted_range / cluster.weight_sum;
        output.elevation_deg =
            cluster.weighted_elevation / cluster.weight_sum;
        output.azimuth_deg = faces::wrap360(
            std::atan2(
                cluster.weighted_sin_azimuth,
                cluster.weighted_cos_azimuth) / kDeg2Rad);
        if (detection.snr_db > output.snr_db) {
            output.snr_db = detection.snr_db;
            output.face_id = detection.face_id;
            output.sim_millis = detection.sim_millis;
        }
        output.contributors.insert(
            output.contributors.end(),
            detection.contributors.begin(),
            detection.contributors.end());
    };

    std::vector<Cluster> clusters;
    clusters.reserve(input.size());
    for (const auto& detection : input) {
        bool fused = false;
        for (auto& cluster : clusters) {
            const auto& current = cluster.representative;
            if (std::llabs(detection.sim_millis - current.sim_millis)
                    > kTimeToleranceMs ||
                std::fabs(detection.range_m - current.range_m)
                    > kRangeToleranceM ||
                wrapped_azimuth_separation_deg(
                    detection.azimuth_deg, current.azimuth_deg)
                    > kAzimuthToleranceDeg ||
                std::fabs(
                    detection.elevation_deg - current.elevation_deg)
                    > kElevationToleranceDeg) {
                continue;
            }

            add_to_cluster(cluster, detection);
            fused = true;
            break;
        }
        if (!fused) {
            Cluster cluster{detection, 0.0, 0.0, 0.0, 0.0, 0.0};
            cluster.representative.contributors.clear();
            add_to_cluster(cluster, detection);
            clusters.push_back(cluster);
        }
    }

    std::vector<FaceDetection> output;
    output.reserve(clusters.size());
    for (const auto& cluster : clusters)
        output.push_back(cluster.representative);
    return output;
}

} // namespace radar::app
