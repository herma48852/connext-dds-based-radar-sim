# Multi-topic live views

These browser prototypes provide the live multi-topic workarounds described in
[the presentation playbook](../../RadarDemoPresentationPlaybook.md#9-live-observability-limitations-and-the-wis-workaround).

| Section | View | Windows launcher |
|---|---|---|
| 4.2 | [Detection to Beam](detection_to_beam_live_view.html) | [`start-wis-detection-beam.cmd`](../../scripts/windows/multi_topic_live_views/start-wis-detection-beam.cmd) |
| 4.3 | [RMA Outage Impact Monitor](rma_outage_impact_live_view.html) | [`start-wis-rma-impact.cmd`](../../scripts/windows/multi_topic_live_views/start-wis-rma-impact.cmd) |
| 4.4 | [Track Association Diagnostics](likely_detection_track_lineage_live_view.html) | [`start-wis-association-diagnostics.cmd`](../../scripts/windows/multi_topic_live_views/start-wis-association-diagnostics.cmd) |
| 4.5 | [Own-Ship Motion Geometry Decomposition](own_ship_motion_geometry_live_view.html) | [`start-wis-motion-geometry.cmd`](../../scripts/windows/multi_topic_live_views/start-wis-motion-geometry.cmd) |
| 4.6 | [Track Coast and Loss Diagnosis](track_coast_loss_live_view.html) | [`start-wis-track-loss.cmd`](../../scripts/windows/multi_topic_live_views/start-wis-track-loss.cmd) |

The pages use five separate single-application service configurations in
[`config/radar_live_view_wis.xml`](../../config/radar_live_view_wis.xml), so
launching one page creates only its DDS participant and readers. Windows has
one `.cmd` launcher per page under `scripts/windows/multi_topic_live_views`,
mirroring this HTML directory; macOS selects the same configuration with
`scripts/start-wis.sh --cfg-name NAME`. See Section 4 for the complete launcher
table.

View 4.4 selects `RecordingDiagnosticsApp`; when connected, WIS creates the
`Recording.DiagnosticTools` participant that consumes the authoritative
`Radar/TrackAssociationEvent` stream.

The prototypes use the current telemetry schema as their baseline:
`DetectionEvent.beam_id` is required by 4.2, and `TrackAssociationEvent` is
required by 4.4 and 4.6. RawReturn transitions may corroborate a beam join and
DetectionEvent samples may enrich an association decision, but the pages do
not reconstruct missing beam identities or tracker decisions.
