# Connext Studio monitoring reference

This system is built so that **Connext Studio** (RTI's VS Code extension,
from RTI Labs) running in a **separate workspace** can dynamically monitor,
visualize and diagnose all DDS traffic in real time. This document is the
lower-level topology, QoS, and diagnostics reference. For the operator's
workspace-switching webinar runbook and AI visualization prompts, see
[`../ConnextStudioDemo.md`](../ConnextStudioDemo.md).

## 1. Setup

1. Start the system:
   ```bash
   ./build/radar_app.app/Contents/MacOS/radar_app --domain 92 &
   ./build/target_gen --domain 92 --targets 32 &
   ```
   (macOS: `radar_app` is a bundle — run the binary inside it, not the
   stale plain `./build/radar_app` path.)
2. Open a **new, separate VS Code window/workspace** (File > New Window).
   It does not need to contain this project — an empty folder works.
3. Open Connext Studio and join **domain 92**. Domain 0 remains the application
   default; the webinar runbook uses 92 to isolate the demo.
4. No extra configuration is needed: discovery is standard Simple
   Discovery over UDPv4, and all types are discoverable through the builtin
   **TypeLookup Service**, so Studio can decode samples without the IDL file.

## 2. What to show, in order

### 2.1 Topology map

Open [`dds_architecture.html`](dds_architecture.html) in a browser and click a
participant to focus its existing readers and writers without changing the
diagram layout. The page scales the diagram as large as possible while keeping
the complete 1600 x 940 canvas visible, including in browser fullscreen mode.
Click again, click the background, or press Escape to reset.

Every component is a named participant:

| Participant name | Role |
|---|---|
| `Radar.BeamScheduler` | publishes four keyed `Radar/BeamCommand` instances (100 Hz/face; 400 Hz aggregate) |
| `Radar.Beamformer` | publishes four keyed `Radar/BeamPatternStatus` instances (20 Hz/face; 80 Hz aggregate); subscribes `Radar/BeamCommand`, `Radar/CalibrationStatus` |
| `Radar.DetectionProcessor` | pub `Radar/RawReturn` and `Radar/DetectionEvent`; sub `Radar/BeamCommand`, `Radar/BeamPatternStatus`, `Radar/RawReturn`, `TargetGen/TargetTruth`, and content-filtered `Ship/ShipPosition` (`source_id = 0`) |
| `Radar.TrackManager` | subscribes `Radar/DetectionEvent` and content-filtered `Ship/ShipPosition` (`source_id = 0`); publishes `Radar/TargetTrack` (10 Hz) and authoritative `Radar/TrackAssociationEvent` decisions |
| `Radar.CalibrationMonitor` | publishes four keyed `Radar/CalibrationStatus` instances (1 Hz/face heartbeat + state changes) |
| `Radar.CommandHandler` | subscribes `Radar/SystemCommand` (WaitSet) |
| `Radar.ShipINS` | publishes `Ship/ShipPosition` (key 0) |
| `Radar.CommandConsole` | publishes `Radar/SystemCommand` (UI buttons) |
| `Radar.HMI-UI` | subscribes `Radar/TargetTrack`, `Radar/DetectionEvent`, content-filtered `Ship/ShipPosition` (`source_id = 0`), `Radar/CalibrationStatus`, `Radar/BeamPatternStatus` — the display endpoint |
| `TargetGen.Generator` | publishes `TargetGen/TargetTruth` + `Ship/ShipPosition` (key 1) |

When the Section 4.4 WIS view is connected, Studio also shows
`Recording.DiagnosticTools`. This participant is created by WIS and consumes
`Radar/TrackAssociationEvent`, `Radar/TargetTrack`, and
`Radar/DetectionEvent`; it is the demo's explicit recording/diagnostic
endpoint rather than part of the radar processing flow. The simplified
architecture overview intentionally omits this optional participant, leaving
`Radar/TrackAssociationEvent` and the `Ship/ShipPosition` instance with
`source_id = 1` ending at the DDS bus as externally consumable diagnostic
hooks.

Note the **loopback edge** inside `Radar.DetectionProcessor`
(RawReturn out and back in)—four face-keyed 1 kHz post-beamforming receiver
streams on the bus (4 kHz aggregate). DetectionProcessor noncoherently
integrates the ten pulses in each 10 ms dwell before publishing CFAR-like
DetectionEvent plots; PRF samples are not independent tracker hits.
Operational flows terminate at an in-system subscriber (the display topics
terminate at `Radar.HMI-UI`). The two deliberate exceptions are
`Radar/TrackAssociationEvent` and `Ship/ShipPosition` with `source_id = 1`;
they terminate at an external diagnostic consumer only when one is connected.
The beam path is also explicit: scheduler intent and array health converge at
`Radar.Beamformer`; its effective response fans out to the receiver model and
display.

The navigation path is equally explicit in discovery: the source-0
`Ship/ShipPosition` instance from `Radar.ShipINS` fans out through DDS
content-filtered readers to DetectionProcessor, TrackManager, and HMI-UI.
The source-1 instance from TargetGen remains available to diagnostic views but
cannot enter operational processing.

### 2.2 Topic tree and live data

Hierarchical names render as a tree: `Radar/...`, `Ship/...`,
`TargetGen/...`. Open live data inspection on:

- `Radar/RawReturn` — watch the aggregate sample rate (~4 kHz), filter by
  `array_id` to see 1 kHz per face, and inspect the 668 complex range cells
  in `iq_samples`
- `Radar/BeamPatternStatus` — inspect the appended representative S-band
  carrier, wavelength, physical pitch, bandwidth, PRF, pulse width, range
  resolution, and unambiguous-range fields alongside the live array pattern
  content; show that Studio decodes it via TypeLookup alone.
- `Ship/ShipPosition` — **two instances** of the same keyed topic:
  `source_id = 0` (radar INS) and `source_id = 1` (ground truth). Show
  per-instance filtering.
- `Radar/CalibrationStatus` — four TRANSIENT_LOCAL keyed instances: a freshly
  joined Studio immediately sees the latest state for every face (durability
  demo). Each includes a per-element drift sequence and `rma_offline_mask`.
- `Radar/BeamPatternStatus` — 20 Hz per-face RELIABLE + TRANSIENT_LOCAL beam
  telemetry published by `Radar.Beamformer`. Chart gain loss, 3 dB width,
  boresight error, and peak sidelobe level; reshape the 181-value azimuth cut
  into a line plot.
- `TargetGen/TargetTruth` — one instance per `target_id`, including the
  deterministic baseline fighter (`target_id = 1`) circling the ship at a
  constant 12 km slant range.

### 2.3 QoS inspection

All QoS comes from named profiles in `qos/radar_qos.xml`; the names are
self-explanatory in Studio's QoS views:

- Compare `DetectionEventProfile` (BEST_EFFORT, 1 ms latency budget) with
  `TargetTrackProfile` (RELIABLE, TRANSIENT_LOCAL, 200 ms deadline).
- The reliability **variety is deliberate**: high-rate sensor paths trade
  reliability for latency; command and track paths are reliable.

### 2.4 Live diagnostic scenarios

When using any command-line form below, stop the normal target generator and
restart it with the shown flag while Studio remains connected. This avoids a
duplicate target/truth publisher.

1. **QoS mismatch** —
   ```bash
   ./build/target_gen --domain 92 --targets 32 --inject-qos-mismatch
   ```
   Creates `TargetGen.RogueReader`: a RELIABLE DataReader on
   `Radar/DetectionEvent` whose writers are BEST_EFFORT. Discovery flags a
   requested/offered incompatibility; use Studio's match analysis / AI
   troubleshooting to explain it, then kill the process to clear it.

2. **Type mismatch** —
   ```bash
   ./build/target_gen --domain 92 --targets 32 --inject-type-mismatch
   ```
   Creates `TargetGen.RogueWriter`: writes type `DetectionEvent` on the
   topic **name** `TargetGen/TargetTruth` (registered type `TargetTruth`).
   Studio reports an inconsistent-topic / type conflict.

3. **Degraded array** — either press **DEGRADE ARRAY** in the radar UI's
   SCENARIOS panel, or:
   ```bash
   ./build/target_gen --domain 92 --targets 32 --degrade-array --face fs
   ```
   Watch `Radar/CalibrationStatus`: `overall_status` goes
   `ARRAY_NOMINAL -> ARRAY_DEGRADED`, `failed_element_count` jumps to
   ~12% of 1024, and the per-element drift sequence shows hard failures.
   Restore with **RESTORE ARRAY**.

4. **RMA offline** — click a block in the radar UI's **ARRAY FACE** pane
   (each block = one Radar Modular Assembly, 64 T/R elements), or:
   ```bash
   ./build/target_gen --domain 92 --targets 32 --rma-offline 3 --face fs
   ```
   Watch `Radar/CalibrationStatus`: `rma_offline_mask` gains the bit,
   `failed_element_count` jumps by 64 per offline RMA, and the drift
   sequence shows that 8×8 block dark. Then open
   `Radar/BeamPatternStatus`: gain falls, the main lobe broadens, and the
   array-factor cut and sidelobe metrics change according to RMA position.
   The B-scope renders the same DDS sample as an automatic overlay. Chart the
   `Radar/DetectionEvent` SNR distribution as RMAs go offline; a strong
   target can occasionally appear through a dominant sidelobe at a displaced
   dwell azimuth. With all 16 RMAs offline, `DetectionEvent` publication
   stops for that face. The other three keyed I/Q streams continue. Existing
   confirmed tracks coast for up to 12 seconds if no other face observes
   them. When the whole selected face is dark, use **ALL ONLINE**; otherwise
   click individual offline blocks. The CLI accepts
   `--face fs|as|ap|fp|all`.

5. **Sector scan** — before issuing the command, create a
   `Radar/BeamCommand` Time Chart and plot `azimuth_deg` for all four keyed
   `scheduler_id` instances. Use `scheduler_id` to distinguish the series; do
   not plot it as a numeric Y field, because that only adds flat lines at
   0 through 3. Select FS and press **SECTOR SCAN**: the FS trace changes from
   a full-face sawtooth to a faster triangular sweep through thirteen centers
   from 31.5 to 58.5 degrees, while the other three faces retain their full
   90-degree sweeps. This makes both the narrower sector, increased revisit
   cadence, and face-local nature of the command visible at once. The B-scope
   shows the nominal 30 and 60 degree sector boundaries. Optionally isolate
   `scheduler_id = 0` afterward for a closer look or chart `priority`
   separately to show its change from 3 to 2.

## 3. Compatibility notes for Studio

- One shared domain (92 in the webinar runbook; 0 by default), standard
  discovery, and UDPv4 transport — Studio sees everything a normal
  participant sees.
- All IDL types are `@appendable` with verbose field names and units in
  comments; future field additions will not break Studio sessions or
  older app versions.
- No DDS-Security and no Persistence/Recording service in play; Recording
  Service can be pointed at the same domain later without code changes.

## 4. Designing hypothetical live multi-topic custom AI views

Connext Studio custom views currently operate on one topic at a time. The
prompts in this section describe what could be built if a custom AI view could
subscribe to several topics and maintain state across their live samples. They
are forward-looking design prompts, not descriptions of a currently available
Studio feature.

Runnable prototypes for Sections 4.2 through 4.6 and their shared browser
assets are grouped in
[`multi_topic_live_views/`](multi_topic_live_views/README.md).

These prompts do not ask an AI to answer a question once. Each prompt asks it
to construct a continuously updating view: subscribe before the event, perform
deterministic stream processing, retain the resulting rows, render tables and
charts, and explain a selected row on demand. A user can inspect an earlier
event only if the view observed and retained it, or if a DDS recording/replay
source supplies it.

### 4.1 Live-view prompt contract

A useful multi-topic prompt must specify all of the following:

1. **Subscriptions:** the topics and instance filters that feed the view.
2. **Working state:** short rolling buffers, per-instance state, event-time
   ordering, and an allowance for samples arriving on different DDS streams in
   a different order.
3. **Materialized history:** compact correlated rows that remain selectable
   after the high-rate source samples have expired from the working buffers.
4. **Deterministic processing:** joins, windowing, coordinate transforms,
   counters, and confidence rules. The language model should explain computed
   evidence; it should not invent joins from a textual sample dump.
5. **Live presentation:** tables, charts, alerts, filters, sorting, and a
   detail panel that update as samples arrive.
6. **Session limits:** what is unknown because the view joined late, lost a
   BEST_EFFORT sample, restarted, or evicted retained history.

Apply these rules to every prompt below:

- Mark the view's subscription time and show it in the UI.
- Use `timestamp.epoch_millis` for joins across processes. `sim_millis` is
  process-relative and is safe only when the compared samples are known to
  originate in the same process.
- Retain DDS metadata needed to interpret the stream: writer GUID, instance
  key, source and reception timestamps, validity, instance state/disposal,
  and sample-loss status.
- Match reader QoS to each topic. `RawReturn`, `DetectionEvent`, and
  `TargetTruth` are BEST_EFFORT and VOLATILE. `BeamCommand` and
  `SystemCommand` are RELIABLE but VOLATILE. Their samples from before the
  view subscribed are not available without recording/replay.
- Treat `CalibrationStatus`, `BeamPatternStatus`, and `ShipPosition` as state
  topics. Use the latest sample at or before an event, and report the age of
  the state used.
- Project high-rate samples into compact metadata before invoking the language
  model. Do not continuously send `RawReturn.iq_samples`,
  `BeamPatternStatus.azimuth_pattern_db`, or
  `CalibrationStatus.element_drift_db` to the model. A deterministic transform
  may inspect those arrays and retain derived values.
- Use a short event-time grace period before finalizing a join because samples
  written on different DDS topics can reach the view in either order.
- Preserve publisher sessions in identifiers. A displayed detection can use
  `detection_id`, but its retained identity should include writer GUID,
  `sensor_id`, and `detection_id`. A track identity should include writer GUID,
  `track_id`, and the DDS instance lifecycle because numeric track IDs can be
  recycled.
- Use authority-appropriate status labels and retain their evidence. Direct
  schema joins use `EXACT`, `NOT_OBSERVED`, or `SCHEMA_ERROR`; reported tracker
  decisions use `REPORTED` or `REPORTED_TRUNCATED`. Use `RECONSTRUCTED_*` only
  in views whose question intentionally requires an inference.
- Never equate “not observed by this view” with “not published” or “not
  received by another DDS reader.”

Face-local identifiers use the following common mapping:

| Face | Name | `scheduler_id` / `array_id` / `sensor_id` |
|---:|---|---:|
| 0 | Forward Starboard (FS) | 0 |
| 1 | Aft Starboard (AS) | 1 |
| 2 | Aft Port (AP) | 2 |
| 3 | Forward Port (FP) | 3 |

The live processing chain is:

```text
SystemCommand -> CalibrationStatus -> BeamPatternStatus
                         |
                         v
BeamCommand -> RawReturn -> DetectionEvent -> TrackManager -> TargetTrack
      ^              ShipPosition + TargetTruth       \
                                                       -> TrackAssociationEvent
                                                          -> Recording.DiagnosticTools
```

### 4.2 Which beam is producing each live detection?

This is not a prompt to inspect one already-expired detection. It constructs a
live, retained detection-to-beam table. Correlation happens as each detection
arrives, while the surrounding raw-return and command samples are still in the
view's buffers.

Use this custom AI view prompt:

```text
Build a live multi-topic view named "Detection-to-Beam Correlation".

Do not produce a one-time answer. Subscribe continuously to:
- Radar/DetectionEvent
- Radar/RawReturn
- Radar/BeamCommand
- Radar/BeamPatternStatus

Show the subscription start time. State that the view can correlate only
detections observed since that time unless recording/replay supplies earlier
samples.

Working state:
- Treat DetectionEvent.beam_id as the authoritative producing-dwell identity.
- Require beam_id. If it is absent, display SCHEMA_ERROR and do not infer a
  replacement beam from timing or pointing.
- Keep a rolling, optional RawReturn metadata buffer for every array_id. Retain
  array_id, beam_id, timestamp.epoch_millis, azimuth_deg, elevation_deg,
  range_bin_count, DDS source/reception order, and sample-loss indicators.
- Do not copy iq_samples into the language-model context. It is not needed to
  find a dwell boundary.
- Keep a rolling BeamCommand buffer containing all scalar fields and DDS
  metadata.
- Keep the latest BeamPatternStatus samples per array_id and beam_id. Project
  scalar pattern metrics; do not continuously copy azimuth_pattern_db.
- Keep pending DetectionEvent rows briefly so BeamCommand and DetectionEvent
  delivery order cannot determine whether the exact command was observed.
- Separately retain completed enriched detection rows for the life of this
  view, subject to an explicitly displayed retention limit.

For each RawReturn.array_id, maintain the current distinct beam_id, its first
and last observed timestamps, pointing angles, and observed sample count.
Detect a dwell boundary when an incoming RawReturn for that array_id has a
different beam_id.

For a transition A -> B:
- A is the completed, producing dwell.
- B is the new dwell whose first processed return caused DetectionProcessor
  to complete A and publish A's detections.
- The transition time is the timestamp of the first observed RawReturn for B.
- Missing BEST_EFFORT pulses may mean the view observes a later pulse of B
  rather than its first pulse.

For every new DetectionEvent:
1. Join BeamCommand exactly where:
     BeamCommand.scheduler_id = DetectionEvent.sensor_id
     BeamCommand.beam_id      = DetectionEvent.beam_id
   Do not select a nearby command if the exact command was not observed.
2. Optionally select RawReturn evidence where:
     RawReturn.array_id = DetectionEvent.sensor_id
     RawReturn.beam_id  = DetectionEvent.beam_id
   A missing RawReturn is a BEST_EFFORT evidence gap, not ambiguous beam
   attribution.
3. When matching RawReturn is observed, locate its A -> B transition to explain
   when DetectionProcessor completed A. Attribute the detection to A, never B.
4. Validate that A's RawReturn azimuth_deg and elevation_deg match the
   DetectionEvent pointing within configured numeric tolerances.
5. Validate the DetectionEvent pointing against the exact BeamCommand. A
   disagreement is a validation warning; it does not change beam identity.
6. Join BeamPatternStatus first by matching array_id and beam_id. Because
   pattern status is 20 Hz while commands are 100 Hz, fall back to the latest
   state for that array_id at or before the producing dwell and label that
   pattern join as AS_OF rather than EXACT.
7. Copy the correlation and its evidence into the retained detection row.
   Do not require the original RawReturn or BeamCommand samples to remain
   available after this row has been materialized.

Use writer GUID + sensor_id + detection_id as the internal row identity.
Display detection_id prominently and sort newest detections first.

Render a continuously updating table with:

Detection:
- sensor_id and face name
- detection_id
- detection epoch_millis
- range_m
- azimuth_deg
- elevation_deg
- amplitude
- snr_db

Producing raw dwell:
- producing_raw_array_id
- producing_raw_beam_id
- dwell_first_raw_epoch_millis
- dwell_last_raw_epoch_millis
- observed_raw_sample_count
- producing_raw_azimuth_deg
- producing_raw_elevation_deg
- transition_to_beam_id
- observed_transition_epoch_millis

Beam command:
- scheduler_id
- beam_id
- command epoch_millis
- commanded azimuth_deg
- commanded elevation_deg
- dwell_time_us
- mode
- priority

Pattern state:
- pattern join type and state age
- rma_offline_mask
- boresight_error_deg
- gain_loss_db
- beamwidth_3db_deg
- peak_sidelobe_level_db

Correlation:
- detection_to_transition_delta_ms
- command_to_dwell_delta_ms
- detection/raw azimuth delta
- detection/raw elevation delta
- attribution confidence
- attribution status
- concise evidence or ambiguity explanation

Add filters for face, detection_id, producing beam_id, mode, SNR, confidence,
and time. Selecting a retained row must open its evidence without rerunning
the correlation from expired source buffers.

Confidence:
- EXACT: DetectionEvent.beam_id joins the BeamCommand on the same face.
  Pointing and RawReturn are independent validation evidence and may raise a
  warning without changing the declared beam identity.
- COMMAND_NOT_OBSERVED: beam_id is authoritative, but the exact BeamCommand
  was not retained after this view subscribed.
- SCHEMA_ERROR: DetectionEvent.beam_id is absent or invalid. Do not attribute
  the detection to any beam.
- Never downgrade an exact DetectionEvent-to-BeamCommand join merely because
  this BEST_EFFORT reader missed the corresponding RawReturn.

Never silently choose the nearest command when candidates are ambiguous.
```

The central join now uses `DetectionEvent.sensor_id + beam_id` to
`BeamCommand.scheduler_id + beam_id`. The key RawReturn transition fields are
still `array_id`, `beam_id`, and `timestamp.epoch_millis`; `azimuth_deg` and
`elevation_deg` validate and explain the dwell boundary. Neither
`range_bin_count` nor `iq_samples` is needed for attribution.

#### Test the HTML prototype with Web Integration Service

[`detection_to_beam_live_view.html`](multi_topic_live_views/detection_to_beam_live_view.html) is a
standalone implementation of this view. The supplied
[`radar_live_view_wis.xml`](../config/radar_live_view_wis.xml) contains an
aggregate `RadarLiveViews` service configuration with one application per
Section 4 solution. Each page enables only its own application and readers on
domain 92. `RawReturn` and `BeamPatternStatus` are projected so their large
arrays are not serialized to JSON.

From the repository root, start RTI Web Integration Service with the launcher
for the current shell.

Windows Command Prompt (`cmd.exe`):

```bat
scripts\windows\start-wis.cmd
```

Windows PowerShell:

```powershell
.\scripts\windows\start-wis.ps1
```

macOS:

```bash
./scripts/start-wis.sh
```

All three launchers use `CONNEXTDDS_DIR` or `NDDSHOME`, select the aggregate
`RadarLiveViews` configuration, serve `docs`, and listen on port 18080. They
resolve repository paths themselves, so they do not depend on the caller's
working directory. Use `scripts\windows\start-wis.cmd -Help`, PowerShell
`Get-Help .\scripts\windows\start-wis.ps1 -Detailed`, or
`./scripts/start-wis.sh --help` for configuration, port, document-root,
verbosity, and built-in-topic overrides.

Run the interactive demo with its default domain 92 if it is not already
running, then open one or more of these pages:

- [`detection_to_beam_live_view.html`](multi_topic_live_views/detection_to_beam_live_view.html) — 4.2
  detection-to-beam attribution;
- [`rma_outage_impact_live_view.html`](multi_topic_live_views/rma_outage_impact_live_view.html) — 4.3
  pre/post RMA outage incidents;
- [`likely_detection_track_lineage_live_view.html`](multi_topic_live_views/likely_detection_track_lineage_live_view.html)
  — 4.4 authoritative track-association diagnostics (`Recording.DiagnosticTools`);
- [`own_ship_motion_geometry_live_view.html`](multi_topic_live_views/own_ship_motion_geometry_live_view.html)
  — 4.5 own-ship/contact motion decomposition;
- [`track_coast_loss_live_view.html`](multi_topic_live_views/track_coast_loss_live_view.html) — 4.6
  coast warning and retained loss diagnosis.

For example, use
`http://localhost:18080/multi_topic_live_views/rma_outage_impact_live_view.html`.
Select **Connect live** and leave the supplied entity names unchanged. The
page enables the configured participant, subscriber, and readers before binding them, so
either launch order works. Volatile evidence begins when the page subscribes;
use **Demo stream** to review any UI without DDS traffic.

One XML file may contain multiple named `web_integration_service` sections,
but `-cfgName` selects one section per WIS process. To make all five pages
available from one process, `RadarLiveViews` instead contains five separate
`application` sections. The older `RadarLiveView` service configuration is
retained as a 4.2-only compatibility option.

The XML domain is intentionally fixed at 92 to match `start-all`. If the
simulation is launched with a different domain, change `domain_id` in the XML
before starting Web Integration Service.

### 4.3 How is a live RMA outage changing radar performance?

This view begins measuring before an outage, detects the actual calibration
transition, and keeps updating its post-event comparison. It should not wait
for a user to ask retrospectively whether an outage mattered.

Use this custom AI view prompt:

```text
Build a live multi-topic view named "RMA Outage Impact Monitor".

Do not produce a one-time answer. Subscribe continuously to:
- Radar/SystemCommand
- Radar/CalibrationStatus
- Radar/BeamPatternStatus
- Radar/BeamCommand
- Radar/DetectionEvent
- optionally TargetGen/TargetTruth
- optionally Ship/ShipPosition with source_id = 0

Maintain:
- current CalibrationStatus and BeamPatternStatus per face;
- a rolling minimum 30-second history of scalar pattern metrics, beam-command
  metadata, detection metadata, and SystemCommand events;
- rolling per-face counters by BeamCommand.mode;
- compact retained incident rows for every rma_offline_mask transition;
- a 15-second pre-event baseline and a 15-second post-event window for each
  incident. Mark a baseline incomplete when the view has not yet run for the
  full pre-event interval.

Treat a CalibrationStatus.rma_offline_mask change as the authoritative outage
or restore event. Correlate a preceding SystemCommand by time and
target_face_mask to show the requested action, but do not treat the command
alone as proof that array state changed.

For every calibration transition:
1. Record face, event time, old mask, new mask, failed_element_count, and
   overall_status.
2. Calculate:
     offline_RMAs    = popcount(rma_offline_mask)
     active_elements = 1024 - 64 * offline_RMAs
3. Confirm whether BeamPatternStatus.rma_offline_mask converges to the new
   calibration mask and record the propagation delay.
4. Track gain_loss_db, beamwidth_3db_deg, boresight_error_deg,
   peak_sidelobe_level_db, and sidelobe offsets before and after the event.
5. Count search opportunities from BeamCommand where
   mode = BEAM_MODE_SEARCH.
6. Count DetectionEvent samples for the same face and interval.
7. Calculate:
     search_detection_yield =
         detections / observed BEAM_MODE_SEARCH commands
   Keep detection rate per second as a secondary metric only.
8. Compare median, 10th percentile, and 90th percentile DetectionEvent.snr_db
   and amplitude.
9. Use unaffected faces as a concurrent control:
     controlled_change =
         affected_face_post_pre_change
         - aggregate_unaffected_face_post_pre_change
10. If TargetTruth is subscribed, compute a second, stronger yield metric
    using only search dwells in which a target was within the commanded beam,
    elevation acceptance, instrumented range, and target-dependent effective
    range. Label this truth-assisted metric separately from observable radar
    telemetry.
11. Update the post-event statistics continuously until the 15-second window
    closes, then freeze a final incident summary while leaving live rolling
    charts active.

Render:
- a current per-face status table with mask, health, active elements, latest
  pattern metrics, search-dwell rate, detection yield, and SNR;
- an incident table sorted newest first, with REQUESTED, OBSERVED,
  COLLECTING, COMPLETE, or INCOMPLETE status;
- synchronized time charts for mask changes, gain, beamwidth, boresight,
  sidelobes, detections per search dwell, and SNR;
- a 4-by-16 RMA mask heatmap;
- a short evidence-based summary that refreshes while an incident is
  COLLECTING and becomes retained when COMPLETE.

Every incident summary must state:
- requested command, if observed;
- actual calibration transition;
- pattern response;
- detection-yield and SNR changes;
- unaffected-face control behavior;
- completeness and confidence.

Do not claim that zero detections proves a dead RawReturn stream. With all
16 RMAs offline, the simulator continues publishing thermal-noise RawReturn
samples but intentionally publishes no detections.

If BEST_EFFORT DetectionEvent or TargetTruth loss is reported, show the loss
indicator and reduce confidence. If the view joined after the outage, display
the current transient-local calibration/pattern state but state that no valid
pre-event comparison exists.
```

The operational question is now “what changed when the state changed while
this view was watching?” A retained incident can be selected later, but the
view cannot manufacture its missing pre-outage baseline after joining late.

### 4.4 Which incoming detections did TrackManager associate with each live track?

`Radar/TrackAssociationEvent` is the TrackManager's authoritative decision
stream. It retains the exact source detection identities through
resolution-cell fusion and publishes the selected lifecycle, fused
measurement, innovation score, decision, and reason. The view therefore
joins reported decisions instead of reimplementing private tracker gates.

The supplied WIS application is `RecordingDiagnosticsApp`. Connecting the
view creates the named `Recording.DiagnosticTools` DomainParticipant, making
the diagnostic consumer and its effect on the topology visible in Studio.

Use this custom AI view prompt:

```text
Build a live multi-topic view named "Track Association Diagnostics".

Do not produce a one-time answer. Subscribe continuously to:
- Radar/TrackAssociationEvent (required, RELIABLE)
- Radar/TargetTrack
- Radar/DetectionEvent (optional source-sample lookup; BEST_EFFORT)

Use tracker_instance_id + track_lifecycle_id as the authoritative lifecycle
identity. Display track_id for operators, but never join only on that recycled
numeric value. Continue to observe TargetTrack DDS instance state and disposal
so the current state table can remove dead instances.

Maintain:
- a retained decision table keyed by tracker_instance_id + association_id;
- current lifecycle-to-TargetTrack state;
- a short optional DetectionEvent cache keyed by sensor_id + detection_id;
- initiation, update, merge, rejection, coast-timeout drop, and reset-drop
  events for each lifecycle;
- subscription start and retention boundaries.

For each TrackAssociationEvent:
1. Materialize the row immediately; do not wait for a TargetTrack sample.
2. Copy every contributing detection identity, including sensor_id,
   detection_id, beam_id, and source timestamp.
3. Copy the fused range, azimuth, elevation, SNR, innovation score, passing
   candidate count, selected lifecycle, confirmation state, and last accepted
   simulation time.
4. Interpret decision and reason together. In particular, distinguish
   capacity rejection, duplicate merge, coast timeout, reset, and orderly
   TrackManager shutdown.
5. If contributors_truncated is true, show REPORTED_TRUNCATED and display
   contributor_count versus the retained bounded sequence length.
6. Join the selected lifecycle to subsequent TargetTrack state. This enriches
   the decision with the public track state; it does not determine or validate
   the association.
7. Optionally join each contributor to a cached DetectionEvent for amplitude,
   ship position, and browser-observed DDS metadata. A missing optional sample
   must be labeled NOT_OBSERVED, not treated as missing tracker evidence.

Render two continuously updating tables.

Active track table:
- lifecycle-scoped track identity and track_id
- last TargetTrack timestamp
- position, velocity, classification, quality
- most recent reported fused measurement
- contributing detection IDs
- innovation score and passing candidate count
- time since accepted measurement
- decision and reason

Retained decision table:
- association_id and event time
- fused measurement values
- all contributing detection and beam identities
- selected and related lifecycle identities
- decision, reason, innovation score, and passing candidate count
- following TargetTrack update time
- REPORTED or REPORTED_TRUNCATED status

Selecting a track must show its reported lifecycle from initiation through
updates, merge, and drop. Selecting a decision must show its exact contributor
identities and optional DetectionEvent lookup results.

Do not rerun the range/cross-range gate in the view or replace the reported
winner with a browser-computed candidate. If TrackAssociationEvent is absent,
show NO_AUTHORITATIVE_ASSOCIATION_STREAM and do not reconstruct decisions.
```

The useful live question is now “which detections did the tracker actually
use, and what decision did it make?” Earlier decisions remain inspectable only
if the view or a recorder observed them because the reliable diagnostic topic
is intentionally VOLATILE.

### 4.5 How much of live contact motion is caused by own-ship motion?

This view continuously decomposes display motion instead of asking generally
how ship motion affects geometry. It makes the current simulator limitation
explicit: heading affects transforms, while published pitch and roll do not
tilt the modeled beam or PPI geometry.

Use this custom AI view prompt:

```text
Build a live multi-topic view named "Own-Ship Motion Geometry Decomposition".

Do not produce a one-time answer. Subscribe continuously to:
- Ship/ShipPosition for source_id = 0 and source_id = 1
- Radar/DetectionEvent
- Radar/TargetTrack
- TargetGen/TargetTruth
- optionally Radar/BeamCommand

Use timestamp.epoch_millis for cross-process joins. Interpolate ShipPosition
to each detection, track, and truth event. Identify track lifecycles using
writer GUID, track_id, and DDS instance state. Treat any TargetTrack-to-
TargetTruth.target_id match as inferred.

Maintain:
- a local tangent-plane own-ship path derived from latitude, longitude, and
  altitude for both INS and truth;
- current and previous ship heading, course, speed, pitch, and roll;
- retained per-track geometry rows and short motion segments;
- INS-minus-truth navigation residuals;
- inferred track-to-truth matches with confidence and ambiguity.

For each TargetTrack sample, derive:
  slant_range = sqrt(east^2 + north^2 + up^2)
  az_world    = wrap360(atan2(east, north))
  elevation   = atan2(up, sqrt(east^2 + north^2))
  az_display  = wrap360(az_world - interpolated_ship_heading)

For every consecutive pair of samples in one track lifecycle:
1. Let r0 be the previous ship-relative ENU vector and r1 the current vector.
2. Let delta_ship be own-ship ENU displacement between the two timestamps.
3. Construct the counterfactual relative vector for a stationary Earth-fixed
   contact:
     r_after_translation = r0 - delta_ship
4. Compute display azimuth in this fixed order:
     a0            = bearing(r0) - old_heading
     a_translation = bearing(r_after_translation) - old_heading
     a_heading     = bearing(r_after_translation) - new_heading
     a1            = bearing(r1) - new_heading
5. Use wrapped angular differences:
     translation_contribution = a_translation - a0
     heading_contribution     = a_heading - a_translation
     contact_motion_residual  = a1 - a_heading
6. Apply the analogous vector decomposition for range:
     observed relative displacement = r1 - r0
     own_ship_translation effect    = -delta_ship
     estimated contact displacement =
         observed relative displacement + delta_ship
7. Retain the interval, source states, components, and propagated track/INS
   uncertainty.

For DetectionEvent, show instantaneous geometry using the interpolated ship
state. Do not claim persistent detection identity unless it has been linked
through the inferred track-lineage view.

Render:
- an Earth-fixed map and ship-relative PPI side by side;
- own-ship course and heading vectors;
- per-contact trails in both frames;
- a current contact table containing range, world bearing, display bearing,
  elevation, translation contribution, heading contribution, estimated
  contact-motion residual, and confidence;
- synchronized charts of heading/course/speed/pitch/roll and selected-contact
  geometry;
- INS-versus-truth position and heading residuals;
- a retained interval-detail panel showing the counterfactual calculation.

For a selected live contact, generate a concise statement such as:
  "During the last interval the contact moved 1.8 degrees clockwise on the
   PPI: 1.5 degrees came from heading change, 0.3 degrees from translation,
   and the remaining change came from estimated contact motion."

Always state:
- DetectionEvent and BeamCommand azimuths are ship-relative.
- TargetTrack and TargetTruth positions are ship-relative ENU.
- Pitch and roll are published and displayed but are not applied by the
  current simulator's detection or display geometry. Their correlation with
  contact motion is not a modeled geometric effect.
- Track-to-truth identity is inferred, not exact.
```

This formulation answers a per-contact, continuously updated operational
question. It also preserves interval rows so the operator can inspect motion
that occurred after the view subscribed.

### 4.6 Why is a live track coasting, and why did it disappear?

A useful live view should warn while a track is coasting, classify the missing
evidence as new illumination opportunities occur, and freeze an incident row
if DDS reports track disposal. Waiting until after disposal to start
subscribing would lose most of the required volatile evidence.

Use this custom AI view prompt:

```text
Build a live multi-topic view named "Track Coast and Loss Diagnosis".

Do not produce a one-time answer. Subscribe continuously to:
- Radar/TrackAssociationEvent (required, RELIABLE)
- Radar/TargetTrack
- Radar/DetectionEvent
- Radar/BeamCommand
- Radar/RawReturn
- Radar/BeamPatternStatus
- Radar/CalibrationStatus
- Radar/SystemCommand
- Ship/ShipPosition filtered to source_id = 0
- TargetGen/TargetTruth when truth-assisted diagnosis is desired

Use tracker_instance_id + track_lifecycle_id from TrackAssociationEvent as the
authoritative lifecycle identity. Also track TargetTrack DDS writer GUID,
validity, and disposal. Retain at
least the preceding 20 seconds of compact evidence because a confirmed track
can coast for approximately 12 seconds after its last accepted measurement.
Do not retain the complete high-rate RawReturn.iq_samples stream in the
language-model context. Derive and retain per-dwell health and expected-range-
cell features deterministically.

Reuse the authoritative decisions from the "Track Association Diagnostics"
view. The timestamp on a repeatedly published TargetTrack is not proof of a
new accepted detection. Set last_accepted_measurement_time only from an
INITIATE or UPDATE TrackAssociationEvent; use its exact contributing
detections for optional source-sample lookup.

For each active track lifecycle:
1. Predict its current ship-relative range, bearing, and elevation.
2. Identify every BeamCommand that should illuminate that geometry, including
   face, elevation bar, mode, dwell time, and beam offset.
3. For each expected visit, join the applicable CalibrationStatus and
   BeamPatternStatus state as of the dwell.
4. Monitor RawReturn metadata for the same face and beam_id. Retain observed
   pulse count, range-bin availability, and stream-loss indicators.
5. When needed, deterministically integrate only the relevant RawReturn range
   cell and neighboring cells across the dwell. Retain peak magnitude,
   local-maximum result, and comparison with the fixed 0.26 threshold rather
   than retaining all I/Q.
6. Look for a DetectionEvent and the reported TrackAssociationEvent decision
   after the dwell.
7. Update coast age, number of expected visits since the last accepted
   measurement, current diagnosis, and confidence.
8. On DDS disposal, freeze the lifecycle and all preceding evidence as a
   retained loss incident. Continue accepting a short grace window of
   late-arriving evidence before finalizing the diagnosis.
9. Use DROP or MERGE TrackAssociationEvent reason as the authoritative
   lifecycle termination cause. A nearby CMD_RESET may corroborate a reported
   RESET reason but must not manufacture one when association telemetry is
   absent.

Classify every missed update using this evidence:

SCHEDULING:
No applicable BeamCommand crossed the predicted target geometry, commonly
after a sector or mode command.

CALIBRATION:
Applicable commands existed, but rma_offline_mask changed or pattern gain,
boresight, beamwidth, or sidelobes degraded while SNR/yield fell.

RECEIVER_OR_DATA_PATH:
Commands and pattern state were valid, but expected RawReturn samples stopped,
were incomplete, or could not form the expected integrated trace. Because
RawReturn is BEST_EFFORT, distinguish source failure from view-local loss when
DDS metadata allows it.

SENSITIVITY_OR_RANGE:
RawReturn traffic was healthy, but the expected peak failed the local-maximum
or 0.26 threshold test because of range, target RCS, beam offset, gain loss, or
noise. Consider the target-dependent effective-range model.

TRACKER_ASSOCIATION:
Detections continued but TrackAssociationEvent reports rejection, fusion into
another measurement, assignment to a competing lifecycle, or initiation of a
replacement lifecycle.

EXPLICIT_RESET:
TrackAssociationEvent reports DROP/RESET. A nearby CMD_RESET is supporting
context only.

NORMAL_COAST_DROP:
A confirmed track was disposed just over 12 seconds after its last reported
accepted measurement, with reason COAST_TIMEOUT. Preserve the upstream reason
that caused the coast;
"normal coast/drop" describes tracker behavior, not the original missed
measurement.

NEAR_RANGE_MODEL:
Below approximately 3 km, a truncated echo can be reported at an
outward-biased apparent range. This can break association and create a
replacement track even when the receiver is behaving as modeled.

INSUFFICIENT_EVIDENCE:
Required volatile history predates the view, BEST_EFFORT loss is material, or
multiple explanations remain equally plausible.

Render:
- an active-track table with lifecycle, track_id, quality, predicted geometry,
  last accepted measurement, coast age, estimated time to drop,
  expected/missed visits, current diagnosis, and confidence;
- a live timeline for the selected track showing commands, calibration and
  pattern state, raw-dwell health, detections, reported associations, track
  publications, commands, and disposal;
- alert cards when a track begins coasting, changes diagnosis, approaches the
  coast limit, or is disposed;
- a retained loss-incident table with final diagnosis, upstream cause,
  disposal time, coast duration, evidence completeness, and confidence;
- a selected-incident explanation that cites the observed samples and missing
  evidence.

Do not force every incident into scheduling, receiver, or calibration.
Tracker association, explicit reset, sensitivity/range, near-range behavior,
normal coast/drop, and insufficient evidence are valid outcomes.

If TargetTruth is used, label the conclusion TRUTH_ASSISTED. Do not imply that
truth was available to the operational tracker.
```

This changes the old retrospective question into two live behaviors: an early
warning for a currently coasting track and a retained incident created when a
track disappears.

### 4.7 Exact telemetry status and remaining extensions

The first exact-join telemetry is now implemented:

1. `DetectionEvent.beam_id` identifies the producing dwell directly.
2. Reliable, volatile `Radar/TrackAssociationEvent` publishes a bounded
   contributor sequence, fused measurement, innovation score, candidate
   count, decision, and lifecycle-scoped track identity.
3. The same decision stream carries lifecycle initiation, duplicate merge,
   coast-timeout drop, reset drop, and shutdown drop, including last accepted
   simulation time and an explicit reason. This avoids a second lifecycle
   topic while keeping the event key space bounded.

The recording/diagnostic WIS participant consumes this stream. Live views
still need subscription-time and retention notices, but beam attribution and
detection-to-track lineage no longer depend on reconstructed timing. The HTML
prototypes treat these fields and events as their required baseline and do not
retain legacy reconstruction paths. A future
recorder can persist `TrackAssociationEvent` unchanged for retrospective
analysis.
