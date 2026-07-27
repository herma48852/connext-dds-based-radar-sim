#include "RadarFaces.hpp"

#include <bit>
#include <cmath>
#include <cstdio>

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

    faces::FaceMask combined_mask = 0u;
    double next_start_deg = 0.0;
    for (int i = 0; i < faces::kFaceCount; ++i) {
        const auto& face = faces::kDefinitions[static_cast<std::size_t>(i)];
        check(face.id == i, "face keys are contiguous 0..3");
        check(face.mask == faces::mask(face.id),
              "face mask matches its bounded DDS key");
        check(std::popcount(face.mask) == 1,
              "each face owns exactly one command-mask bit");
        check(std::fabs(face.coverage_start_deg - next_start_deg) < 1.0e-12,
              "adjacent face fields meet without an azimuth gap");
        check(std::fabs(
                  face.coverage_end_deg - face.coverage_start_deg - 90.0)
                  < 1.0e-12,
              "each face covers exactly 90 degrees");
        check(std::fabs(
                  face.boresight_deg -
                  0.5 * (face.coverage_start_deg + face.coverage_end_deg))
                  < 1.0e-12,
              "face boresight bisects its field of regard");
        combined_mask |= face.mask;
        next_start_deg = face.coverage_end_deg;
    }

    check(std::fabs(next_start_deg - 360.0) < 1.0e-12,
          "four faces cover the full 360-degree ship-relative azimuth");
    check(combined_mask == faces::kAllFacesMask,
          "all four face bits combine to the bounded all-faces mask");
    check(faces::normalize_target_mask(0u)
              == faces::kForwardStarboardMask,
          "zero command mask retains legacy forward-starboard behavior");
    check(faces::normalize_target_mask(0xF0u)
              == faces::kForwardStarboardMask,
          "unknown-only command masks cannot target a nonexistent face");
    check(faces::normalize_target_mask(
              faces::kForwardPortMask | faces::kAftPortMask)
              == (faces::kForwardPortMask | faces::kAftPortMask),
          "valid multi-face command masks are preserved");
    check(faces::targets(faces::kAllFacesMask, faces::kAftStarboard),
          "all-faces command targets an individual face");
    check(!faces::targets(
              faces::kForwardPortMask, faces::kForwardStarboard),
          "single-face command does not leak to another aperture");
    check(!faces::valid(-1) && !faces::valid(faces::kFaceCount),
          "face keys outside 0..3 are rejected");
    check(faces::for_azimuth(0.0) == faces::kForwardStarboard &&
              faces::for_azimuth(89.999) == faces::kForwardStarboard,
          "forward-starboard owns azimuths below the 90-degree seam");
    check(faces::for_azimuth(90.0) == faces::kAftStarboard &&
              faces::for_azimuth(180.0) == faces::kAftPort &&
              faces::for_azimuth(270.0) == faces::kForwardPort,
          "clockwise face owns each shared lower boundary");
    check(faces::for_azimuth(360.0) == faces::kForwardStarboard &&
              faces::for_azimuth(-0.1) == faces::kForwardPort,
          "face lookup wraps cleanly across north");
    check(faces::contains(faces::kAftPort, 225.0) &&
              !faces::contains(faces::kAftPort, 315.0),
          "contains applies the canonical bounded face geometry");

    if (failures != 0) {
        std::fprintf(stderr, "radar_faces_regression: %d failure(s)\n",
                     failures);
        return 1;
    }
    std::printf(
        "radar_faces_regression: PASS "
        "(FS 045, AS 135, AP 225, FP 315; mask 0x%02X)\n",
        static_cast<unsigned>(faces::kAllFacesMask));
    return 0;
}
