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
   ./build/target_gen --domain 92 --targets 16 &
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
| `Radar.BeamScheduler` | publishes `Radar/BeamCommand` (100 Hz) |
| `Radar.Beamformer` | publishes `Radar/BeamPatternStatus` (20 Hz); subscribes `Radar/BeamCommand`, `Radar/CalibrationStatus` |
| `Radar.DetectionProcessor` | pub `Radar/RawReturn` and `Radar/DetectionEvent`; sub `Radar/BeamCommand`, `Radar/BeamPatternStatus`, `Radar/RawReturn`, `TargetGen/TargetTruth` |
| `Radar.TrackManager` | publishes `Radar/TargetTrack` (10 Hz) |
| `Radar.CalibrationMonitor` | publishes `Radar/CalibrationStatus` (1 Hz heartbeat + state changes) |
| `Radar.CommandHandler` | subscribes `Radar/SystemCommand` (WaitSet) |
| `Radar.ShipINS` | publishes `Ship/ShipPosition` (key 0) |
| `Radar.CommandConsole` | publishes `Radar/SystemCommand` (UI buttons) |
| `Radar.HMI-UI` | subscribes `Radar/TargetTrack`, `Radar/DetectionEvent`, `Ship/ShipPosition` (key 0), `Radar/CalibrationStatus`, `Radar/BeamPatternStatus` — the display endpoint |
| `TargetGen.Generator` | publishes `TargetGen/TargetTruth` + `Ship/ShipPosition` (key 1) |

Note the **loopback edge** inside `Radar.DetectionProcessor`
(RawReturn out and back in) — the 1 kHz receiver wire on the bus.
Every topic has at least one in-system subscriber (the display topics
terminate at `Radar.HMI-UI`), so there are no dangling publishers.
The beam path is also explicit: scheduler intent and array health converge at
`Radar.Beamformer`; its effective response fans out to the receiver model and
display.

### 2.2 Topic tree and live data

Hierarchical names render as a tree: `Radar/...`, `Ship/...`,
`TargetGen/...`. Open live data inspection on:

- `Radar/RawReturn` — watch the sample rate (~1 kHz) and `iq_samples`
  content; show that Studio decodes it via TypeLookup alone.
- `Ship/ShipPosition` — **two instances** of the same keyed topic:
  `source_id = 0` (radar INS) and `source_id = 1` (ground truth). Show
  per-instance filtering.
- `Radar/CalibrationStatus` — TRANSIENT_LOCAL: a freshly joined Studio
  immediately sees the last sample (durability demo). Includes the
  per-element drift sequence and `rma_offline_mask` (bit per RMA).
- `Radar/BeamPatternStatus` — 20 Hz RELIABLE + TRANSIENT_LOCAL beam
  telemetry published by `Radar.Beamformer`. Chart gain loss, 3 dB width,
  boresight error, and peak sidelobe level; reshape the 181-value azimuth cut
  into a line plot.
- `TargetGen/TargetTruth` — one instance per `target_id`.

### 2.3 QoS inspection

All QoS comes from named profiles in `qos/radar_qos.xml`; the names are
self-explanatory in Studio's QoS views:

- Compare `DetectionEventProfile` (BEST_EFFORT, 1 ms latency budget) with
  `TargetTrackProfile` (RELIABLE, TRANSIENT_LOCAL, 100 ms deadline).
- The reliability **variety is deliberate**: high-rate sensor paths trade
  reliability for latency; command and track paths are reliable.

### 2.4 Live diagnostic scenarios

When using any command-line form below, stop the normal target generator and
restart it with the shown flag while Studio remains connected. This avoids a
duplicate target/truth publisher.

1. **QoS mismatch** —
   ```bash
   ./build/target_gen --domain 92 --targets 16 --inject-qos-mismatch
   ```
   Creates `TargetGen.RogueReader`: a RELIABLE DataReader on
   `Radar/DetectionEvent` whose writers are BEST_EFFORT. Discovery flags a
   requested/offered incompatibility; use Studio's match analysis / AI
   troubleshooting to explain it, then kill the process to clear it.

2. **Type mismatch** —
   ```bash
   ./build/target_gen --domain 92 --targets 16 --inject-type-mismatch
   ```
   Creates `TargetGen.RogueWriter`: writes type `DetectionEvent` on the
   topic **name** `TargetGen/TargetTruth` (registered type `TargetTruth`).
   Studio reports an inconsistent-topic / type conflict.

3. **Degraded array** — either press **DEGRADE ARRAY** in the radar UI's
   SCENARIOS panel, or:
   ```bash
   ./build/target_gen --domain 92 --targets 16 --degrade-array
   ```
   Watch `Radar/CalibrationStatus`: `overall_status` goes
   `ARRAY_NOMINAL -> ARRAY_DEGRADED`, `failed_element_count` jumps to
   ~12% of 1024, and the per-element drift sequence shows hard failures.
   Restore with **RESTORE ARRAY**.

4. **RMA offline** — click a block in the radar UI's **ARRAY FACE** pane
   (each block = one Radar Modular Assembly, 64 T/R elements), or:
   ```bash
   ./build/target_gen --domain 92 --targets 16 --rma-offline 3  # or "all"
   ```
   Watch `Radar/CalibrationStatus`: `rma_offline_mask` gains the bit,
   `failed_element_count` jumps by 64 per offline RMA, and the drift
   sequence shows that 8×8 block dark. Then open
   `Radar/BeamPatternStatus`: gain falls, the main lobe broadens, and the
   array-factor cut and sidelobe metrics change according to RMA position.
   The B-scope renders the same DDS sample as an automatic overlay. Chart the
   `Radar/DetectionEvent` SNR distribution as RMAs go offline; a strong
   target can occasionally appear through a dominant sidelobe at a displaced
   dwell azimuth. Restore with **ALL ONLINE**.

5. **Sector scan** — press **SECTOR SCAN** in the UI and watch
   `Radar/BeamCommand`: azimuth values bounce between 60 and 120 deg
   instead of wrapping 0..360. The B-scope shows dashed sector boundary
   lines.

## 3. Compatibility notes for Studio

- One shared domain (92 in the webinar runbook; 0 by default), standard
  discovery, and UDPv4 transport — Studio sees everything a normal
  participant sees.
- All IDL types are `@appendable` with verbose field names and units in
  comments; future field additions will not break Studio sessions or
  older app versions.
- No DDS-Security and no Persistence/Recording service in play; Recording
  Service can be pointed at the same domain later without code changes.
