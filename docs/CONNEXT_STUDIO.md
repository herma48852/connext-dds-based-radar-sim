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

## 4. Hypothetical multi-topic custom AI views

Connext Studio custom views currently operate on one topic at a time. This
section describes questions that could be answered if a custom AI view could
subscribe to, retain, and correlate samples from multiple topics. The design
is intentionally forward-looking; it is not a description of a currently
available Studio feature.

The AI should perform deterministic joins and calculations first, then use the
language model to explain the evidence. Every result should be labeled as
**exact**, **reconstructed**, or **inferred**.

### 4.1 Common correlation model

A multi-topic view should:

- Subscribe before the event and retain a rolling history. `BeamCommand`,
  `RawReturn`, `DetectionEvent`, and `TargetTruth` are VOLATILE, so a
  late-joining view cannot reconstruct their earlier activity.
- Use `timestamp.epoch_millis` for cross-process alignment. `sim_millis` is
  process-relative and is unsafe when samples from `radar_app` and
  `target_gen` are combined.
- Retain DDS metadata: writer GUID, instance key, source and reception
  timestamps, validity, instance state/disposal, and sample-loss status.
- Use as-of joins for state topics: select the latest `CalibrationStatus`,
  `BeamPatternStatus`, and `ShipPosition` sample at or before the event.
- Match each reader's QoS to the topic. `RawReturn`, `DetectionEvent`, and
  `TargetTruth` are BEST_EFFORT; a sample missing from the view does not prove
  that another reader, such as TrackManager, did not receive it.
- Keep track identities lifecycle-scoped. `TargetTrack.track_id` values come
  from a bounded, recycled pool, so the same numeric ID can represent a later
  track after disposal and re-registration.

Face-local fields share one bounded identity:

| Face | Name | `scheduler_id` / `array_id` / `sensor_id` |
|---:|---|---:|
| 0 | Forward Starboard (FS) | 0 |
| 1 | Aft Starboard (AS) | 1 |
| 2 | Aft Port (AP) | 2 |
| 3 | Forward Port (FP) | 3 |

The correlated processing chain is:

```text
SystemCommand -> CalibrationStatus -> BeamPatternStatus
                         |
                         v
BeamCommand -> RawReturn -> DetectionEvent -> TargetTrack
      ^              ShipPosition + TargetTruth
```

### 4.2 Which beam command produced this detection?

Subscribe to:

- `Radar/DetectionEvent`
- `Radar/RawReturn`
- `Radar/BeamCommand`
- `Radar/BeamPatternStatus`

For a selected detection:

1. Read its `sensor_id`, timestamp, azimuth, and elevation.
2. Find the immediately preceding completed `RawReturn` dwell for the same
   face.
3. Detect the `RawReturn.beam_id` transition. DetectionProcessor integrates
   ten 1 kHz returns and completes the previous dwell when the first return of
   the next beam arrives.
4. Treat the previous `RawReturn.beam_id` as the producing dwell.
5. Join it to `BeamCommand` using:

   ```text
   BeamCommand.scheduler_id = DetectionEvent.sensor_id
   BeamCommand.beam_id      = inferred RawReturn.beam_id
   ```

6. Verify that the command and detection azimuth/elevation agree.
7. Join `BeamPatternStatus` by face and `beam_id` when that beam was published.
   Pattern status is 20 Hz while commands are 100 Hz, so some dwells require
   an as-of join to the latest pattern state on that face.

An answer should identify the face, beam ID, mode, priority, pointing,
dwell duration, and applied pattern state. For example:

> Detection 4812 was produced by inferred search dwell 938 on the
> Forward-Starboard face, commanded to azimuth 22.5 degrees and elevation
> 14 degrees. Ten raw pulses were integrated. The applied pattern had
> 1.8 dB gain loss and 0.4 degrees of boresight error.

This is normally a high-confidence reconstruction, not an exact join:
`DetectionEvent` does not currently contain `beam_id`. Missing BEST_EFFORT raw
samples or repeated pointings can leave more than one candidate. Adding
`beam_id` to `DetectionEvent` would make the lineage explicit.

### 4.3 Did an RMA outage change the beam pattern and detection rate?

Subscribe to:

- `Radar/SystemCommand`
- `Radar/CalibrationStatus`
- `Radar/BeamPatternStatus`
- `Radar/BeamCommand`
- `Radar/DetectionEvent`
- optionally `TargetGen/TargetTruth` and `Ship/ShipPosition`

The analysis should:

1. Find the outage request in `SystemCommand`, including its
   `target_face_mask`.
2. Confirm the actual transition in `CalibrationStatus`, using
   `rma_offline_mask`, `failed_element_count`, and `overall_status`.
3. Confirm that `BeamPatternStatus.rma_offline_mask` reflects the calibration
   state.
4. Calculate the number of offline RMAs and active elements:

   ```text
   offline_RMAs  = popcount(rma_offline_mask)
   active_elements = 1024 - 64 * offline_RMAs
   ```

5. Chart `gain_loss_db`, `beamwidth_3db_deg`, `boresight_error_deg`, peak
   sidelobe level, and sidelobe offsets.
6. Compare equal pre-outage and post-outage windows, such as 15 seconds each.
7. Normalize detection count by illumination opportunity:

   ```text
   detection_yield = detections / applicable search dwells
   ```

   Detections per second alone are misleading if sector or scan mode changes.
8. Compare median and percentile SNR, not only detection count. A moderate
   outage may reduce SNR without immediately suppressing detections.
9. Use unaffected faces as a control group:

   ```text
   outage_effect = affected-face post/pre change
                 - unaffected-face post/pre change
   ```

10. For the strongest analysis, use `TargetTruth` to count opportunities when
    a target was inside the commanded beam, elevation gate, instrumented
    range, and target-dependent effective range.

The view should combine an outage/restore event line, RMA mask heatmap, beam
metrics, detections per search dwell, SNR distribution, and confirmed-track
count. A useful conclusion would be:

> After RMA 3 went offline, gain fell 1.2 dB, beamwidth increased 6%, and
> median SNR fell 1.4 dB. Detection yield declined from 0.31 to 0.24 per
> eligible search dwell, while unaffected faces remained within 2%.

With all 16 RMAs offline, raw thermal noise remains on that face's I/Q stream,
but DetectionProcessor intentionally publishes no detections.

### 4.4 Which detections contributed to a particular track?

Subscribe to:

- `Radar/DetectionEvent`
- `Radar/TargetTrack`
- `Ship/ShipPosition`, filtered to `source_id = 0`
- optionally `TargetGen/TargetTruth`

This question requires reconstruction because the current tracker does not
publish detection-to-track provenance.

1. Buffer detections in the same 100 ms batches used by TrackManager.
2. Reproduce resolution-cell fusion. Reports are grouped when they are within:

   - 30 ms;
   - 1.5 range cells, approximately 225 m;
   - 3.3 degrees in azimuth; and
   - 0.1 degrees in elevation-bar center.

   Fused range and bearing are weighted by linear power derived from SNR.
3. Interpolate own-ship heading at the batch time.
4. Convert each ship-relative polar measurement to ENU:

   ```text
   az_world = az_ship + ship_heading
   horizontal_range = range * cos(elevation)

   east  = horizontal_range * sin(az_world)
   north = horizontal_range * cos(az_world)
   up    = range * sin(elevation)
   ```

5. Predict each candidate track to the batch time.
6. Reproduce the association gates:

   - range gate: at least 375 m, widened by SNR-derived measurement and motion
     uncertainty;
   - azimuth gate: at least 2.6 degrees, widened by measurement uncertainty;
   - cross-range gate: a 150 m floor plus range-scaled angular and motion
     uncertainty.

7. Compute the normalized range/cross-range innovation score and select the
   lowest valid candidate.
8. Associate the fused measurement with the following 10 Hz `TargetTrack`
   update.
9. Backtrack through initiation. A track is confirmed only after three
   independent visits, at least 600 ms apart, inside a six-second window.

The view could present the inferred lineage as:

```text
Detection IDs 4102 + 4103
        |
        v  resolution-cell fusion, power weighted
Measurement: 12.15 km, 43.1 degrees, SNR 17.8 dB
        |
        v  association score 0.18
Track 1004 update at 14:32:08.400
```

This cannot be exact with the current schema. `detection_id` is discarded
when reports enter resolution-cell fusion, and `TargetTrack` contains no
contributing IDs. Studio's BEST_EFFORT reader can also receive a different
sample subset from TrackManager's reader.

An exact implementation would publish a bounded `TrackAssociationEvent` with
the track ID, source detection IDs, fused measurement, innovation score, and
association result such as initiate, update, reject, merge, coast, or drop.

### 4.5 How does ship motion affect displayed target geometry?

Subscribe to:

- `Ship/ShipPosition`, keys 0 and 1;
- `Radar/DetectionEvent`;
- `Radar/TargetTrack`;
- `TargetGen/TargetTruth`; and
- optionally `Radar/BeamCommand`.

The view should separate translation, heading, and attitude effects.

**Translation:** `TargetTruth.position` is produced relative to the moving
ship: target_gen subtracts the ship's east/north position. As the ship moves,
a target's relative range and bearing change even when its Earth-fixed motion
does not. An Earth-fixed path can be reconstructed by adding interpolated
own-ship displacement back to the target-relative ENU vector.

**Heading:** detections and beam commands use ship-relative azimuth. Tracks
and truth use east/north/up axes relative to the ship. The PPI bearing is:

```text
az_world   = atan2(east, north)
az_display = wrap360(az_world - ship_heading)
```

A heading change rotates the PPI without necessarily changing slant range.

**Pitch and roll:** the simulator publishes pitch and roll, but the current
detection and display coordinate transforms use heading only. Pitch and roll
appear in the ship panel but do not currently tilt the beam or PPI target
geometry. The AI should state this explicitly rather than attributing
displayed movement to seaway attitude.

A useful view would show an Earth-fixed map and ship-relative PPI side by
side, own-ship course and heading vectors, INS-versus-truth residuals, and a
decomposition such as:

> The contact moved 1.8 degrees clockwise on the PPI: 1.5 degrees came from
> the ship's heading change and 0.3 degrees from relative translation. Pitch
> and roll had no modeled geometric effect.

Mapping a `TargetTrack` to a `TargetTruth.target_id` remains inferred because
tracks intentionally do not carry truth IDs.

### 4.6 Did a track disappear because of receiver behavior, scheduling, or calibration state?

Subscribe to:

- `Radar/TargetTrack`
- `Radar/DetectionEvent`
- `Radar/BeamCommand`
- `Radar/RawReturn`
- `Radar/BeamPatternStatus`
- `Radar/CalibrationStatus`
- `Radar/SystemCommand`
- `Ship/ShipPosition`
- `TargetGen/TargetTruth`

For a selected track lifecycle:

1. Detect the track's DDS disposal.
2. Find its last likely associated detection.
3. Predict its range, bearing, and elevation for the following 12 seconds.
4. Identify every expected illumination opportunity for the appropriate face
   and elevation bar.
5. Classify the evidence:

| Diagnosis | Expected evidence |
|---|---|
| Scheduling | No applicable `BeamCommand` crossed the predicted target geometry, often after a sector or mode command. |
| Calibration | Applicable commands existed, but calibration masks changed and pattern gain/pointing degraded; SNR and detection yield fell concurrently. |
| Receiver/data path | Commands and pattern were valid, but `RawReturn` stopped, was incomplete, or failed to produce the expected integrated peak. |
| Sensitivity/range | Raw-return traffic was healthy, but the target peak fell below the fixed 0.26 detector threshold because of range, RCS, beam offset, or gain loss. |
| Tracker association | Detections continued, but fell outside the gate, fused into another measurement, or initiated a replacement track. |
| Explicit reset | A `CMD_RESET` immediately preceded disposal of all tracks. |
| Normal coast/drop | The track disappeared just over 12 seconds after its last accepted measurement. |

For receiver diagnosis, the view could reproduce ten-pulse integration from
`RawReturn`, inspect the expected range cell, and apply the same local-maximum
and fixed-threshold tests used by DetectionProcessor.

The near-range model must also be considered. Below approximately 3 km, a
truncated echo can be reported at an outward-biased apparent range. That can
break association and initiate a replacement track even when the receiver is
working as modeled.

An evidence-based answer could be:

> Track 1004 was last updated at 14:32:08.4 and was disposed 12.1 seconds
> later. Six expected search visits occurred. Beam commands and raw-return
> traffic remained present, but RMA mask `0xFFFF` made the aperture offline;
> pattern gain collapsed and no detections were published. Classification:
> calibration-induced loss followed by normal tracker coast/drop. Confidence:
> high.

The classifier should include tracker behavior as a fourth causal category
rather than forcing every disappearance into receiver, scheduling, or
calibration.

### 4.7 Telemetry additions for exact answers

Multi-topic views would enable strong operational inference with the current
topics. Three small telemetry additions would make the most important joins
exact:

1. Add `beam_id` to `DetectionEvent`.
2. Publish a bounded `TrackAssociationEvent` containing contributing
   detection IDs and the association decision.
3. Publish a track lifecycle/drop-reason event distinguishing coast timeout,
   reset, duplicate merge, and capacity rejection.

Without these additions, outage-impact and ship-motion analyses can be highly
reliable. Beam attribution and track provenance should always include an
explicit confidence level.
