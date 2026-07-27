#pragma once
// ============================================================================
// Canonical ship-relative radar-face geometry and bounded DDS key space.
//
// The numeric keys intentionally match FACE_* and FACE_MASK_* in
// idl/radar_types.idl. Keeping this portable header independent of generated
// DDS code lets geometry and command-routing invariants be regression tested
// without a Connext installation.
// ============================================================================

#include <array>
#include <cstdint>
#include <string_view>

namespace radar::faces {

using FaceId = int32_t;
using FaceMask = uint32_t;

inline constexpr FaceId kForwardStarboard = 0;
inline constexpr FaceId kAftStarboard = 1;
inline constexpr FaceId kAftPort = 2;
inline constexpr FaceId kForwardPort = 3;
inline constexpr int32_t kFaceCount = 4;

inline constexpr FaceMask kForwardStarboardMask = 1u << kForwardStarboard;
inline constexpr FaceMask kAftStarboardMask = 1u << kAftStarboard;
inline constexpr FaceMask kAftPortMask = 1u << kAftPort;
inline constexpr FaceMask kForwardPortMask = 1u << kForwardPort;
inline constexpr FaceMask kAllFacesMask =
    kForwardStarboardMask | kAftStarboardMask |
    kAftPortMask | kForwardPortMask;

struct FaceDefinition {
    FaceId id;
    FaceMask mask;
    std::string_view short_name;
    std::string_view display_name;
    double coverage_start_deg;
    double boresight_deg;
    double coverage_end_deg;
};

// Clockwise ship-relative order: bow is 0 degrees and starboard is 90.
inline constexpr std::array<FaceDefinition, kFaceCount> kDefinitions{{
    {kForwardStarboard, kForwardStarboardMask, "FS",
     "Forward Starboard", 0.0, 45.0, 90.0},
    {kAftStarboard, kAftStarboardMask, "AS",
     "Aft Starboard", 90.0, 135.0, 180.0},
    {kAftPort, kAftPortMask, "AP",
     "Aft Port", 180.0, 225.0, 270.0},
    {kForwardPort, kForwardPortMask, "FP",
     "Forward Port", 270.0, 315.0, 360.0},
}};

constexpr bool valid(FaceId face_id) noexcept {
    return face_id >= 0 && face_id < kFaceCount;
}

constexpr FaceMask mask(FaceId face_id) noexcept {
    return valid(face_id) ? FaceMask{1u} << face_id : FaceMask{0u};
}

constexpr const FaceDefinition* find(FaceId face_id) noexcept {
    return valid(face_id)
        ? &kDefinitions[static_cast<std::size_t>(face_id)]
        : nullptr;
}

// An absent appendable member is zero when received from an older writer.
// Treat zero (and masks containing only unknown future bits) as the original
// forward-starboard face so mixed-version demos retain legacy behavior.
constexpr FaceMask normalize_target_mask(FaceMask requested) noexcept {
    const FaceMask bounded = requested & kAllFacesMask;
    return bounded == 0u ? kForwardStarboardMask : bounded;
}

constexpr bool targets(FaceMask requested, FaceId face_id) noexcept {
    return (normalize_target_mask(requested) & mask(face_id)) != 0u;
}

constexpr double wrap360(double angle_deg) noexcept {
    while (angle_deg >= 360.0) angle_deg -= 360.0;
    while (angle_deg < 0.0) angle_deg += 360.0;
    return angle_deg;
}

// Face boundaries are shared. The clockwise face owns its lower boundary;
// 360 degrees wraps to the Forward Starboard face at zero.
constexpr FaceId for_azimuth(double azimuth_deg) noexcept {
    const double az = wrap360(azimuth_deg);
    if (az < 90.0) return kForwardStarboard;
    if (az < 180.0) return kAftStarboard;
    if (az < 270.0) return kAftPort;
    return kForwardPort;
}

constexpr bool contains(FaceId face_id, double azimuth_deg) noexcept {
    return valid(face_id) && for_azimuth(azimuth_deg) == face_id;
}

static_assert(kDefinitions.front().coverage_start_deg == 0.0);
static_assert(kDefinitions.back().coverage_end_deg == 360.0);
static_assert(kAllFacesMask == 0x0Fu);

} // namespace radar::faces
