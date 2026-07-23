#pragma once
// ============================================================================
// Canonical topic and QoS profile names.
// Hierarchical topic names ("Radar/DetectionEvent") render as a meaningful
// tree in Connext Studio's topology and topic views.
// ============================================================================

namespace radar::dds_names {

// ---- Topics ---------------------------------------------------------------
inline constexpr const char* TOPIC_DETECTION_EVENT   = "Radar/DetectionEvent";
inline constexpr const char* TOPIC_TARGET_TRACK      = "Radar/TargetTrack";
inline constexpr const char* TOPIC_BEAM_COMMAND      = "Radar/BeamCommand";
inline constexpr const char* TOPIC_BEAM_PATTERN_STATUS= "Radar/BeamPatternStatus";
inline constexpr const char* TOPIC_CALIBRATION_STATUS= "Radar/CalibrationStatus";
inline constexpr const char* TOPIC_SYSTEM_COMMAND    = "Radar/SystemCommand";
inline constexpr const char* TOPIC_RAW_RETURN        = "Radar/RawReturn";
inline constexpr const char* TOPIC_SHIP_POSITION     = "Ship/ShipPosition";
inline constexpr const char* TOPIC_TARGET_TRUTH      = "TargetGen/TargetTruth";

// ---- QoS library + profiles (must match qos/radar_qos.xml) ----------------
inline constexpr const char* QOS_LIBRARY             = "RadarQosLibrary";

inline constexpr const char* PROFILE_RADAR_PARTICIPANT   = "RadarQosLibrary::RadarParticipantProfile";
inline constexpr const char* PROFILE_TARGETGEN_PARTICIPANT = "RadarQosLibrary::TargetGeneratorParticipantProfile";
inline constexpr const char* PROFILE_DETECTION_EVENT     = "RadarQosLibrary::DetectionEventProfile";
inline constexpr const char* PROFILE_TARGET_TRACK        = "RadarQosLibrary::TargetTrackProfile";
inline constexpr const char* PROFILE_BEAM_COMMAND        = "RadarQosLibrary::BeamCommandProfile";
inline constexpr const char* PROFILE_BEAM_PATTERN_STATUS = "RadarQosLibrary::BeamPatternStatusProfile";
inline constexpr const char* PROFILE_CALIBRATION_STATUS  = "RadarQosLibrary::CalibrationStatusProfile";
inline constexpr const char* PROFILE_SYSTEM_COMMAND      = "RadarQosLibrary::SystemCommandProfile";
inline constexpr const char* PROFILE_TARGET_TRUTH        = "RadarQosLibrary::TargetTruthProfile";
inline constexpr const char* PROFILE_SHIP_POSITION       = "RadarQosLibrary::ShipPositionProfile";
inline constexpr const char* PROFILE_RAW_RETURN          = "RadarQosLibrary::RawReturnProfile";

} // namespace radar::dds_names
