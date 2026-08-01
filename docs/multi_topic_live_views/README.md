# Multi-topic live views

These browser prototypes implement the live multi-topic view designs in
[Section 4 of the Connext Studio guide](../CONNEXT_STUDIO.md#4-designing-hypothetical-live-multi-topic-custom-ai-views).

| Section | View |
|---|---|
| 4.2 | [Detection to Beam](detection_to_beam_live_view.html) |
| 4.3 | [RMA Outage Impact Monitor](rma_outage_impact_live_view.html) |
| 4.4 | [Track Association Diagnostics](likely_detection_track_lineage_live_view.html) |
| 4.5 | [Own-Ship Motion Geometry Decomposition](own_ship_motion_geometry_live_view.html) |
| 4.6 | [Track Coast and Loss Diagnosis](track_coast_loss_live_view.html) |

The pages use the aggregate `RadarLiveViews` service configuration in
[`config/radar_live_view_wis.xml`](../../config/radar_live_view_wis.xml). See
Section 4 for Windows and macOS launch commands.

View 4.4 selects `RecordingDiagnosticsApp`; when connected, WIS creates the
`Recording.DiagnosticTools` participant that consumes the authoritative
`Radar/TrackAssociationEvent` stream.

The prototypes use the current telemetry schema as their baseline:
`DetectionEvent.beam_id` is required by 4.2, and `TrackAssociationEvent` is
required by 4.4 and 4.6. RawReturn transitions may corroborate a beam join and
DetectionEvent samples may enrich an association decision, but the pages do
not reconstruct missing beam identities or tracker decisions.
