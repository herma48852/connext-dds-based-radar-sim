# Connext Studio monitoring guide

This system is built so that **Connext Studio** (RTI's VS Code extension,
from RTI Labs) running in a **separate workspace** can dynamically monitor,
visualize and diagnose all DDS traffic in real time. This document is the
webinar runbook.

## 1. Setup

1. Start the system:
   ```bash
   ./build/radar_app &
   ./build/target_gen --targets 8 &
   ```
2. Open a **new, separate VS Code window/workspace** (File > New Window).
   It does not need to contain this project — an empty folder works.
3. Open Connext Studio and join **domain 0** (the default for both apps;
   `--domain N` changes it).
4. No extra configuration is needed: discovery is standard Simple
   Discovery over UDPv4 (+ shared memory on the same host), and all types
   are discoverable through the builtin **TypeLookup Service**, so Studio
   can decode samples without the IDL file.

## 2. What to show, in order

### 2.1 Topology map

Every component is a named participant:

| Participant name | Role |
|---|---|
| `Radar.BeamScheduler` | publishes `Radar/BeamCommand` (50 Hz) |
| `Radar.DetectionProcessor` | pub `Radar/RawReturn` + `Radar/DetectionEvent`; sub `Radar/BeamCommand`, `Radar/RawReturn`, `TargetGen/TargetTruth` |
| `Radar.TrackManager` | publishes `Radar/TargetTrack` (10 Hz) |
| `Radar.CalibrationMonitor` | publishes `Radar/CalibrationStatus` (1 Hz) |
| `Radar.CommandHandler` | subscribes `Radar/SystemCommand` (WaitSet) |
| `Radar.ShipINS` | publishes `Ship/ShipPosition` (key 0) |
| `Radar.CommandConsole` | publishes `Radar/SystemCommand` (UI buttons) |
| `Radar.HMI-UI` | subscribes `Radar/TargetTrack`, `Radar/DetectionEvent`, `Ship/ShipPosition` (key 0), `Radar/CalibrationStatus` — the display endpoint |
| `TargetGen.Generator` | publishes `TargetGen/TargetTruth` + `Ship/ShipPosition` (key 1) |

Note the **loopback edge** inside `Radar.DetectionProcessor`
(RawReturn out and back in) — the 1 kHz receiver wire on the bus.
Every topic has at least one in-system subscriber (the display topics
terminate at `Radar.HMI-UI`), so there are no dangling publishers.

### 2.2 Topic tree and live data

Hierarchical names render as a tree: `Radar/...`, `Ship/...`,
`TargetGen/...`. Open live data inspection on:

- `Radar/RawReturn` — watch the sample rate (~1 kHz) and `iq_samples`
  content; show that Studio decodes it via TypeLookup alone.
- `Ship/ShipPosition` — **two instances** of the same keyed topic:
  `source_id = 0` (radar INS) and `source_id = 1` (ground truth). Show
  per-instance filtering.
- `Radar/CalibrationStatus` — TRANSIENT_LOCAL: a freshly joined Studio
  immediately sees the last sample (durability demo).
- `TargetGen/TargetTruth` — one instance per `target_id`.

### 2.3 QoS inspection

All QoS comes from named profiles in `qos/radar_qos.xml`; the names are
self-explanatory in Studio's QoS views:

- Compare `DetectionEventProfile` (BEST_EFFORT, 1 ms latency budget) with
  `TargetTrackProfile` (RELIABLE, TRANSIENT_LOCAL, 100 ms deadline).
- The reliability **variety is deliberate**: high-rate sensor paths trade
  reliability for latency; command and track paths are reliable.

### 2.4 Live diagnostic scenarios

Run these while Studio is connected:

1. **QoS mismatch** —
   ```bash
   ./build/target_gen --inject-qos-mismatch
   ```
   Creates `TargetGen.RogueReader`: a RELIABLE DataReader on
   `Radar/DetectionEvent` whose writers are BEST_EFFORT. Discovery flags a
   requested/offered incompatibility; use Studio's match analysis / AI
   troubleshooting to explain it, then kill the process to clear it.

2. **Type mismatch** —
   ```bash
   ./build/target_gen --inject-type-mismatch
   ```
   Creates `TargetGen.RogueWriter`: writes type `DetectionEvent` on the
   topic **name** `TargetGen/TargetTruth` (registered type `TargetTruth`).
   Studio reports an inconsistent-topic / type conflict.

3. **Degraded array** — either press **DEGRADE ARRAY** in the radar UI's
   SCENARIOS panel, or:
   ```bash
   ./build/target_gen --degrade-array
   ```
   Watch `Radar/CalibrationStatus`: `overall_status` goes
   `ARRAY_NOMINAL -> ARRAY_DEGRADED`, `failed_element_count` jumps to
   ~12% of 1024, and the per-element drift sequence shows hard failures.
   Restore with **RESTORE ARRAY**.

4. **Sector scan** — press **SECTOR SCAN** in the UI and watch
   `Radar/BeamCommand`: azimuth values bounce between 60 and 120 deg
   instead of wrapping 0..360. The B-scope shows dashed sector boundary
   lines.

## 3. Compatibility notes for Studio

- Single domain (0), standard discovery, builtin transports only — Studio
  sees everything a normal participant sees.
- All IDL types are `@appendable` with verbose field names and units in
  comments; future field additions will not break Studio sessions or
  older app versions.
- No DDS-Security and no Persistence/Recording service in play; Recording
  Service can be pointed at the same domain later without code changes.
