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

Every component is a named participant:

| Participant name | Role |
|---|---|
| `Radar.BeamScheduler` | publishes four keyed `Radar/BeamCommand` instances (100 Hz/face; 400 Hz aggregate) |
| `Radar.Beamformer` | publishes four keyed `Radar/BeamPatternStatus` instances (20 Hz/face; 80 Hz aggregate); subscribes `Radar/BeamCommand`, `Radar/CalibrationStatus` |
| `Radar.DetectionProcessor` | pub `Radar/RawReturn` and `Radar/DetectionEvent`; sub `Radar/BeamCommand`, `Radar/BeamPatternStatus`, `Radar/RawReturn`, `TargetGen/TargetTruth` |
| `Radar.TrackManager` | publishes `Radar/TargetTrack` (10 Hz) |
| `Radar.CalibrationMonitor` | publishes four keyed `Radar/CalibrationStatus` instances (1 Hz/face heartbeat + state changes) |
| `Radar.CommandHandler` | subscribes `Radar/SystemCommand` (WaitSet) |
| `Radar.ShipINS` | publishes `Ship/ShipPosition` (key 0) |
| `Radar.CommandConsole` | publishes `Radar/SystemCommand` (UI buttons) |
| `Radar.HMI-UI` | subscribes `Radar/TargetTrack`, `Radar/DetectionEvent`, `Ship/ShipPosition` (key 0), `Radar/CalibrationStatus`, `Radar/BeamPatternStatus` — the display endpoint |
| `TargetGen.Generator` | publishes `TargetGen/TargetTruth` + `Ship/ShipPosition` (key 1) |

Note the **loopback edge** inside `Radar.DetectionProcessor`
(RawReturn out and back in)—four face-keyed 1 kHz post-beamforming receiver
streams on the bus (4 kHz aggregate). DetectionProcessor noncoherently
integrates the ten pulses in each 10 ms dwell before publishing CFAR-like
DetectionEvent plots; PRF samples are not independent tracker hits.
Every topic has at least one in-system subscriber (the display topics
terminate at `Radar.HMI-UI`), so there are no dangling publishers.
The beam path is also explicit: scheduler intent and array health converge at
`Radar.Beamformer`; its effective response fans out to the receiver model and
display.

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

5. **Sector scan** — select FS, press **SECTOR SCAN**, and filter
   `Radar/BeamCommand` to `scheduler_id = 0`: the thirteen commanded centers
   bounce from 31.5 to 58.5 degrees instead of sweeping the full 0..90 face
   field. The B-scope shows the nominal 30 and 60 degree sector boundaries.
   Other faces continue their independent schedules.

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
- Label results `EXACT`, `RECONSTRUCTED_HIGH`, `RECONSTRUCTED_MEDIUM`,
  `RECONSTRUCTED_LOW`, or `UNRESOLVED`, and retain the evidence behind the
  label.
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
BeamCommand -> RawReturn -> DetectionEvent -> TargetTrack
      ^              ShipPosition + TargetTruth
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
- Keep a rolling RawReturn metadata buffer for every array_id. Retain
  array_id, beam_id, timestamp.epoch_millis, azimuth_deg, elevation_deg,
  range_bin_count, DDS source/reception order, and sample-loss indicators.
- Do not copy iq_samples into the language-model context. It is not needed to
  find a dwell boundary.
- Keep a rolling BeamCommand buffer containing all scalar fields and DDS
  metadata.
- Keep the latest BeamPatternStatus samples per array_id and beam_id. Project
  scalar pattern metrics; do not continuously copy azimuth_pattern_db.
- Keep pending DetectionEvent rows briefly so RawReturn and DetectionEvent
  delivery order cannot determine the join result.
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
1. Select the RawReturn stream where:
     RawReturn.array_id = DetectionEvent.sensor_id
2. Find the closest plausible A -> B transition around the detection event
   time. Normally the transition timestamp is equal to or just before the
   DetectionEvent timestamp. Permit an observed transition up to one 10 ms
   dwell after the detection when the first B pulses may have been lost by
   this view.
3. Attribute the detection to A, never to B.
4. Validate that A's RawReturn azimuth_deg and elevation_deg match the
   DetectionEvent pointing within configured numeric tolerances.
5. Join the producing dwell to BeamCommand exactly on:
     BeamCommand.scheduler_id = RawReturn.array_id
     BeamCommand.beam_id      = A
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
- RECONSTRUCTED_HIGH: the A -> B transition was observed, A joins exactly to
  BeamCommand by face and beam_id, and pointing/timing agree.
- RECONSTRUCTED_MEDIUM: the producing dwell is identifiable but some raw
  pulses or pattern evidence are missing.
- RECONSTRUCTED_LOW: attribution depends mainly on time/pointing proximity or
  a transition was partially missed.
- UNRESOLVED: no single producing dwell is defensible.
- Do not label attribution EXACT because DetectionEvent does not carry
  beam_id.

Never silently choose the nearest command when candidates are ambiguous.
```

The key `RawReturn` transition fields are `array_id`, `beam_id`, and
`timestamp.epoch_millis`. `azimuth_deg` and `elevation_deg` validate the
result. Neither `range_bin_count` nor `iq_samples` is needed to identify the
boundary. Adding `beam_id` to `DetectionEvent` would turn the central
attribution into an exact join.

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

### 4.4 Which incoming detections are likely updating each live track?

The current schema does not publish detection-to-track provenance. This view
therefore maintains a live reconstruction and stores the inferred association
at the time it is made. Its title and labels must say “likely” rather than
presenting the result as authoritative tracker output.

Use this custom AI view prompt:

```text
Build a live multi-topic view named "Likely Detection-to-Track Lineage".

Do not produce a one-time answer. Subscribe continuously to:
- Radar/DetectionEvent
- Radar/TargetTrack
- Ship/ShipPosition filtered to source_id = 0
- optionally TargetGen/TargetTruth for evaluation only

Track writer GUID, DDS instance state, and disposal. Define a track lifecycle
as writer GUID + track_id + the interval from instance appearance until
disposal. Never attach retained detections from an old lifecycle to a recycled
numeric track_id.

Maintain:
- DetectionEvent samples in event-time order with a late-arrival grace period;
- the latest own-ship state and an interpolation buffer;
- active track state and prediction history;
- compact retained fused-measurement and inferred-association rows;
- at least six seconds of initiation evidence and more than twelve seconds of
  confirmed-track coast evidence.

Every 100 ms, reproduce the TrackManager observation batch:
1. Group detections into the same resolution cells when they are within:
   - 30 ms in time;
   - 1.5 range cells, approximately 225 m;
   - 3.3 degrees in azimuth; and
   - 0.1 degrees in elevation-bar center.
2. Fuse each group using linear-power weighting derived from SNR. Retain every
   contributing writer GUID, sensor_id, and detection_id in the view's fused
   row.
3. Interpolate own-ship heading to the measurement time.
4. Convert the fused ship-relative measurement to ENU:
     az_world         = az_ship + ship_heading
     horizontal_range = range * cos(elevation)
     east             = horizontal_range * sin(az_world)
     north            = horizontal_range * cos(az_world)
     up               = range * sin(elevation)
5. Predict every candidate track to the batch time.
6. Apply the simulator's association gates:
   - range gate has a 375 m floor and widens with SNR-derived measurement and
     motion uncertainty;
   - azimuth gate has a 2.6 degree floor and widens with uncertainty;
   - cross-range gate has a 150 m floor plus range-scaled angular and motion
     uncertainty.
7. Calculate the normalized range/cross-range innovation score and retain all
   candidates that pass, not only the winner.
8. Choose the lowest-score candidate as the view's likely association.
9. Compare that association with the following live TargetTrack updates.
10. Reconstruct initiation evidence: three independent visits separated by at
    least 600 ms inside a six-second window are required for confirmation.

Render two continuously updating tables.

Active track table:
- lifecycle-scoped track identity and track_id
- last TargetTrack timestamp
- position, velocity, classification, quality
- likely most recent fused measurement
- contributing detection IDs
- association score
- time since likely accepted measurement
- lineage confidence and ambiguity status

Retained association table:
- batch time
- fused measurement values
- all contributing detection identities
- candidate track lifecycles and scores
- selected likely track or INITIATION/REJECTED
- following TargetTrack update time
- confidence and evidence

Selecting a track must show a live lineage timeline from initiation through
updates and coast. Selecting a retained association must show its source
detections and all passing candidate tracks.

Confidence:
- RECONSTRUCTED_HIGH: the view observed every contributing detection, one
  candidate clearly passed with the lowest score, and the following track
  update was consistent.
- RECONSTRUCTED_MEDIUM: the association is unique but BEST_EFFORT loss or
  state interpolation weakens the evidence.
- RECONSTRUCTED_LOW: multiple candidates passed, the following track update
  was ambiguous, or batch membership is uncertain.
- UNRESOLVED: no defensible association can be reconstructed.

Never label lineage EXACT. Detection IDs are discarded before tracker output,
TargetTrack contains no contributing IDs, and this view's BEST_EFFORT reader
may observe a different subset than TrackManager. TargetTruth may evaluate an
inference, but it must not be presented as evidence used by the operational
tracker.
```

The useful live question is “which detections are most likely feeding this
track now?” Earlier retained associations remain inspectable only because the
view materialized them while their source samples were available.

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
- Radar/TargetTrack
- Radar/DetectionEvent
- Radar/BeamCommand
- Radar/RawReturn
- Radar/BeamPatternStatus
- Radar/CalibrationStatus
- Radar/SystemCommand
- Ship/ShipPosition filtered to source_id = 0
- TargetGen/TargetTruth when truth-assisted diagnosis is desired

Track DDS writer GUID, instance lifecycle, validity, and disposal. Retain at
least the preceding 20 seconds of compact evidence because a confirmed track
can coast for approximately 12 seconds after its last accepted measurement.
Do not retain the complete high-rate RawReturn.iq_samples stream in the
language-model context. Derive and retain per-dwell health and expected-range-
cell features deterministically.

Reuse the live detection-fusion and likely-association reconstruction from the
"Likely Detection-to-Track Lineage" view. The timestamp on a repeatedly
published TargetTrack is not proof of a new accepted detection. Maintain a
separate inferred last_accepted_measurement_time.

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
6. Look for a DetectionEvent and a likely track association after the dwell.
7. Update coast age, number of expected visits since the last likely accepted
   measurement, current diagnosis, and confidence.
8. On DDS disposal, freeze the lifecycle and all preceding evidence as a
   retained loss incident. Continue accepting a short grace window of
   late-arriving evidence before finalizing the diagnosis.
9. Detect CMD_RESET separately. Disposal of all track instances immediately
   after reset is an explicit reset, not a receiver failure.

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
Detections continued but were outside the reconstructed gates, fused into
another measurement, assigned to a competing track, or initiated a
replacement lifecycle.

EXPLICIT_RESET:
CMD_RESET immediately preceded broad track disposal.

NORMAL_COAST_DROP:
A confirmed track was disposed just over 12 seconds after its last likely
accepted measurement. Preserve the upstream reason that caused the coast;
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
  last likely accepted measurement, coast age, estimated time to drop,
  expected/missed visits, current diagnosis, and confidence;
- a live timeline for the selected track showing commands, calibration and
  pattern state, raw-dwell health, detections, inferred associations, track
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

### 4.7 Telemetry that would make live views exact

Stateful multi-topic views can provide strong operational reconstructions, but
retaining more samples does not remove schema ambiguity. Three bounded
telemetry additions would convert the most important inferences into exact
live joins:

1. Add the producing `beam_id` to `DetectionEvent`.
2. Publish a bounded `TrackAssociationEvent` containing the lifecycle-scoped
   track identity, contributing detection identities, fused measurement,
   innovation score, and decision such as initiate, update, reject, merge,
   coast, or drop.
3. Publish a track lifecycle event with the last accepted measurement time and
   a drop reason distinguishing coast timeout, reset, duplicate merge, and
   capacity rejection.

With those additions, the live views would still need subscription-time and
retention notices, but beam attribution and detection-to-track lineage would
no longer depend on reconstructed timing.
