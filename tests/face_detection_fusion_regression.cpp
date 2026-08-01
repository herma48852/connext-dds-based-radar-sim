#include "FaceDetectionFusion.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

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
    using radar::app::FaceDetection;
    using radar::app::fuse_resolution_cell_detections;

    const std::vector<FaceDetection> seam_pair{
        {faces::kForwardStarboard, 1000, 30000.0, 88.875, 3.0, 18.0},
        {faces::kAftStarboard, 1004, 30120.0, 91.125, 3.0, 22.0}};
    const auto fused =
        fuse_resolution_cell_detections(seam_pair);
    check(fused.size() == 1,
          "simultaneous adjacent-face reports fuse at a shared seam");
    check(fused.front().face_id == faces::kAftStarboard &&
              std::fabs(fused.front().snr_db - 22.0) < 1.0e-12,
          "fusion retains the stronger face as plot provenance");
    check(fused.front().azimuth_deg > 88.875 &&
              fused.front().azimuth_deg < 91.125,
          "SNR weighting estimates bearing between adjacent beam centers");

    auto provenance_pair = seam_pair;
    provenance_pair[0].contributors.push_back(
        {faces::kForwardStarboard, 41, 9001, 100000, 1000});
    provenance_pair[1].contributors.push_back(
        {faces::kAftStarboard, 42, 9002, 100004, 1004});
    const auto provenance_fused =
        fuse_resolution_cell_detections(provenance_pair);
    check(provenance_fused.size() == 1 &&
              provenance_fused.front().contributors.size() == 2,
          "fusion preserves every contributing detection identity");
    check(provenance_fused.front().contributors[0].detection_id == 41 &&
              provenance_fused.front().contributors[1].beam_id == 9002,
          "fusion provenance retains source detection and beam ids");

    auto same_face = seam_pair;
    same_face[1].face_id = faces::kForwardStarboard;
    check(fuse_resolution_cell_detections(same_face).size() == 1,
          "unresolved adjacent-dwell reports from one face form one plot");

    auto separated_range = seam_pair;
    separated_range[1].range_m += 300.0;
    check(fuse_resolution_cell_detections(separated_range).size() == 2,
          "range-resolvable targets remain separate");

    auto separated_time = seam_pair;
    separated_time[1].sim_millis += 100;
    check(fuse_resolution_cell_detections(separated_time).size() == 2,
          "non-simultaneous cross-face reports remain separate");

    const std::vector<FaceDetection> north_wrap{
        {faces::kForwardPort, 2000, 20000.0, 358.875, 14.0, 16.0},
        {faces::kForwardStarboard, 2001, 20100.0, 1.125, 14.0, 17.0}};
    const auto north_fused =
        fuse_resolution_cell_detections(north_wrap);
    check(north_fused.size() == 1,
          "fusion handles the 359-to-1-degree wraparound seam");
    check(north_fused.front().azimuth_deg < 1.125 ||
              north_fused.front().azimuth_deg > 358.875,
          "circular bearing centroid remains near north at wraparound");

    if (failures != 0) {
        std::fprintf(
            stderr,
            "face_detection_fusion_regression: %d failure(s)\n",
            failures);
        return 1;
    }
    std::printf(
        "face_detection_fusion_regression: PASS "
        "(resolution-cell plots fused and centered)\n");
    return 0;
}
