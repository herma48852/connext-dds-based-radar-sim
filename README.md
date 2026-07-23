# AesaRadarSim

A radar simulation system modeled on publicly available **AESA SPY-6 class**
phased-array radar architecture, built on **RTI Connext DDS 7.7.0 LTS** and
modern **C++20**. Designed as a live webinar demo: **Connext Studio** (RTI's
VS Code extension) monitors, visualizes and diagnoses every DDS sample in a
separate workspace.

> **Windows 11 users:** Start with the
> **[Windows 11 Clean-Machine Runbook](docs/RUN_WINDOWS.md)**. It gives one
> complete, numbered path from installing prerequisites and cloning the
> repository through building, testing, and launching the demo.
>
> **Display presenters:** Use the
> **[Radar Display Operator Guide](docs/OPERATOR_DISPLAY_GUIDE.md)** for panel
> meanings, coordinate frames, colors, controls, and common audience
> questions.

Two applications, one CMake monorepo, one DDS domain (default: domain 0):

| App          | Purpose |
|--------------|---------|
| `radar_app`  | Simulated radar on a moving ship. Internal components (BeamScheduler, Beamformer, DetectionProcessor, TrackManager, CalibrationMonitor, CommandHandler, CommandConsole, HMI-UI) communicate **exclusively via DDS topics**. ImGui UI (native Metal on macOS, OpenGL 3.3 elsewhere) with PPI, A-scope, B-scope, track list, beam timeline, health, ship and ARRAY FACE panels (click an RMA block to take it offline). |
| `target_gen` | Synthetic target generator (configurable trajectories, RCS, kinematics) publishing `TargetGen/TargetTruth` + ship-motion ground truth. Can inject QoS/type mismatches and the degraded-array scenario on demand. |

```
AesaRadarSim/
├── CMakeLists.txt               # macOS-first, Windows-ready build
├── cmake/                       # toolchain files (arm64 macOS, MSVC x64)
├── idl/radar_types.idl          # @appendable types, module radar::types
├── qos/radar_qos.xml            # single QoS file, 10 named profiles
├── src/common/                  # DDS bootstrap, SPSC queue, sim clock
├── src/radar_app/
│   ├── components/              # one class per radar component, one
│   │                            # DomainParticipant each (topology demo)
│   ├── ui/                      # PpiView, AScopeView, BScopeView, Panels
│   └── main.cpp
├── src/target_gen/              # DDS adapter + testable target scenario core
├── scripts/run-demo.sh          # one-command Bash demo launcher
├── tests/                       # headless UI, target, and tracker regressions
├── ConnextStudioDemo.md         # live webinar workspace-switching runbook
└── docs/CONNEXT_STUDIO.md       # monitoring / diagnostics demo guide
```

---

## Prerequisites (macOS, Apple Silicon)

1. **RTI Connext DDS 7.7.0 LTS** with the `arm64Darwin20clang12.0` target
   installed, e.g. at `/Applications/rti_connext_dds-7.7.0`.
2. CMake >= 3.20 (`brew install cmake`), Xcode Command Line Tools.
3. Git (GLFW / Dear ImGui / ImPlot are pulled by CMake FetchContent at
   configure time — no vcpkg or manual dependency management needed).

## Build (macOS)

```bash
export CONNEXTDDS_DIR=/Applications/rti_connext_dds-7.7.0
cmake -B build \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-macos-arm64.cmake \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
```

The QoS file is copied next to the binaries automatically
(`build/qos/radar_qos.xml`). Override at runtime with `RADAR_QOS_FILE`.

## Regression tests

The default build registers four fast, headless CTest regressions:

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
```

- `ui_controls_smoke` renders the production ImGui A-scope and panels in
  memory, performs real mouse press/hold/release frames, and verifies all
  scenario controls (including the local **BEAM FORMATION** toggle), manual
  RMA offline/online, and **ALL ONLINE**. The A-scope
  azimuth/elevation changes throughout to cover the focus-loss regression.
- `target_scenario_regression` accelerates 30 minutes of the production
  16-target webinar scenario and checks stable IDs/profile mix, bounded motion,
  deterministic seeded behavior, the missile altitude floor, and periodic
  120 km respawns.
- `tracker_replay_regression` converts the existing deterministic detection →
  tracker replay into a failing test using golden event counts and track-ID
  pool bounds. Running `./build/tracker_replay [seconds]` directly retains its
  original periodic diagnostic output; assertions are enabled only by CTest's
  `--self-test` flag.
- `beam_pattern_regression` verifies nominal beamwidth, outage gain loss,
  symmetric-error cancellation, mask-position sensitivity, and safe
  all-offline behavior without DDS or a display.

These tests create no DDS participants, graphics window, or renderer, so they
do not alter or compete with a live webinar run.

## Run

The Bash launcher starts both applications, selects either the preset or
legacy build automatically, configures the Connext runtime and QoS paths,
writes separate logs, and stops both processes together:

```bash
./scripts/run-demo.sh --domain 92 --targets 16

# Unattended DDS-only run:
./scripts/run-demo.sh --domain 92 --targets 16 --headless --run-seconds 20
```

It uses `CONNEXTDDS_DIR` or `NDDSHOME`, and on macOS also detects the default
`/Applications/rti_connext_dds-7.7.0` installation. Close the radar window or
press Ctrl-C in the launcher to stop both processes cooperatively.

Launcher options:

| Option | Default | Description |
|--------|---------|-------------|
| `--build-dir PATH` | Auto-detect | Use a specific CMake build directory instead of searching `build/macos-arm64`, `build`, and the repository installation layout. |
| `--connext-dir PATH` | Environment/auto-detect | Use a specific RTI Connext installation instead of `CONNEXTDDS_DIR`, `NDDSHOME`, or the default macOS installation. |
| `--domain N` | `92` | Select DDS domain `0..232`. |
| `--targets N` | `16` | Launch `target_gen` with `1..256` targets. |
| `--run-seconds N` | `0` | Stop automatically after `N` seconds (`0..604800`); zero runs until window close or Ctrl-C. |
| `--headless` | Off | Run `radar_app` without the graphics window. |
| `-h`, `--help` | — | Print launcher usage and exit. |

To run the applications manually in separate terminals:

```bash
# Terminal 1 — the radar console (opens the GUI)
# (use the rtisetenv script matching YOUR Connext target architecture)
# NOTE: radar_app is built as a macOS bundle — the plain ./build/radar_app
# path is a stale pre-bundle leftover and is NEVER relinked. Run the binary
# inside the bundle (it also keeps stdout, which the bare .app does not):
source $CONNEXTDDS_DIR/resource/scripts/rtisetenv_arm64Darwin23clang16.0.bash
./build/radar_app.app/Contents/MacOS/radar_app

# Terminal 2 — the target generator
# Live webinars use exactly 16 targets (two repeats of the eight-profile mix).
# Targets fly inbound and are recycled past 120 km so the picture stays busy;
# tune with --respawn-range KM, 0 disables.
source $CONNEXTDDS_DIR/resource/scripts/rtisetenv_arm64Darwin23clang16.0.bash
./build/target_gen --targets 16

# Diagnostic scenarios (combinable):
./build/target_gen --inject-qos-mismatch     # RELIABLE reader vs BEST_EFFORT writer
./build/target_gen --inject-type-mismatch    # wrong type on TargetGen/TargetTruth
./build/target_gen --degrade-array           # sends CMD_DEGRADE_ARRAY at t+5s
./build/target_gen --rma-offline 3           # sends CMD_RMA_OFFLINE (RMA 3) at t+5s
                                             # ("all" = whole face; restore via the
                                             # ARRAY FACE pane's ALL ONLINE button)
```

> On macOS, Connext shared libraries are resolved via `@rpath`; sourcing
> `rtisetenv_*.bash` (or exporting `DYLD_LIBRARY_PATH` to the target `lib`
> directory) is required before launching.

Both apps accept `--domain N` (default 0). The radar UI also has a
**SCENARIOS** panel (bottom-right). Search/sector mode, degrade/restore array,
self test, and track reset issue `Radar/SystemCommand`s. **BEAM FORMATION**
is a local display toggle: it replaces the compact moving outage curtain with
an animated, slowly rotating 3D comparison: nominal moves to the far left when
an outage occurs, the degraded pattern appears at center, both rotate together
for direct shape comparison, and nominal recenters on recovery.
The **ARRAY FACE** panel issues `CMD_RMA_OFFLINE`/`CMD_RMA_ONLINE`:
click an RMA block (16 blocks of 64 T/R elements) to toggle it, or
**ALL ONLINE** to restore. Offline RMAs darken the block, set the bit
in `CalibrationStatus.rma_offline_mask`. `Radar.Beamformer` combines that
health state with `BeamCommand`, publishes the effective response as
`BeamPatternStatus`, and thereby reduces implant gain, reshapes the beam
according to outage geometry, and activates the compact outage overlay on the
B-scope.

> **macOS note (shared memory):** the shipped profiles use **UDPv4 only**.
> macOS defaults allow very few System V shared-memory segments, and the
> radar app's nine participants exhaust them (RTI KB
> [osx510](http://community.rti.com/kb/osx510)), which otherwise ends in
> "No index available for participant" errors. If you raise the sysv
> limits per that KB, you can switch the transport masks back to
> `UDPv4 | SHMEM` in `qos/radar_qos.xml` — no rebuild needed, QoS is
> loaded at runtime.

## Windows 11 port (Visual Studio 2022)

For a clean Windows 11 machine, follow
**[docs/RUN_WINDOWS.md](docs/RUN_WINDOWS.md) from Step 1 without skipping
steps**. Enter its commands in a regular Command Prompt (`cmd.exe`). The
runbook clones the repository into a parent folder selected by the user,
changes to the repository root once, verifies every prerequisite, and keeps
all setup, build, test, and launch commands in that directory.

- Connext target: `x64Win64VS2017` (binary-compatible with VS2022).
- The UI embeds Per-Monitor V2 DPI awareness and uses GLFW/OpenGL 3.3.
- `windows-portable` builds all four regressions without Connext.
- FetchContent supplies pinned GLFW, ImGui, and ImPlot sources.
- Put the Connext target DLL directory on `PATH` before running.

---

## Architecture

![DDS topic flow: participants, topics and the data bus](docs/dds_architecture.png)
([vector source](docs/dds_architecture.svg))

Every internal radar component is a named DomainParticipant wired to the
others purely through topics on the shared bus — there are no direct
in-process calls between components. `Radar.BeamScheduler` publishes desired
pointing, while `Radar.Beamformer` combines it with
`Radar/CalibrationStatus` and owns the actual outage-aware beam response.
Both `Radar.DetectionProcessor` and the **HMI-UI** consume that response from
`Radar/BeamPatternStatus`. The **HMI-UI** participant is the
display endpoint: it subscribes to `Radar/TargetTrack`,
`Radar/DetectionEvent`, `Ship/ShipPosition`, `Radar/CalibrationStatus`, and
`Radar/BeamPatternStatus` (the B-scope degradation overlay),
so every panel renders data that arrived over the bus — no dangling
publishers anywhere in the system. Its listener callbacks only convert
samples into view structs in a `DataBus` (lock-free SPSC queues +
mutex-protected stores), which the render thread drains at display rate:
the GUI can never stall a DDS receive thread, and DDS threads never touch
OpenGL. (Connext Studio joins the same domain from a
separate workspace and can read every topic shown; see
[docs/CONNEXT_STUDIO.md](docs/CONNEXT_STUDIO.md). Not shown: the
on-demand diagnostic endpoints `target_gen` creates with
`--inject-qos-mismatch`, `--inject-type-mismatch` and `--degrade-array`.)

### DDS topics

| Topic | Type | Rate | Profile | Notes |
|---|---|---|---|---|
| `Radar/RawReturn` | RawReturn | 1 kHz | RawReturnProfile | BEST_EFFORT, 500us latency budget. The "receiver wire", looped back inside DetectionProcessor |
| `Radar/DetectionEvent` | DetectionEvent | ~100 Hz | DetectionEventProfile | BEST_EFFORT CFAR blips; consumed by TrackManager and HMI-UI (PPI) |
| `Radar/BeamCommand` | BeamCommand | 100 Hz | BeamCommandProfile | RELIABLE dwell schedule; consumed by Beamformer and DetectionProcessor |
| `Radar/BeamPatternStatus` | BeamPatternStatus | 20 Hz | BeamPatternStatusProfile | Beamformer-owned RELIABLE + TRANSIENT_LOCAL outage metrics and 181-sample azimuth cut; consumed by DetectionProcessor (return synthesis) and HMI-UI (B-scope overlay) |
| `Radar/TargetTrack` | TargetTrack | 10 Hz | TargetTrackProfile | RELIABLE + TRANSIENT_LOCAL + 100 ms deadline; consumed by HMI-UI (track list) |
| `Radar/CalibrationStatus` | CalibrationStatus | 1 Hz + changes | CalibrationStatusProfile | array health: 1024-element drift + `rma_offline_mask`; consumed by Beamformer and HMI-UI (health + ARRAY FACE panels) |
| `Radar/SystemCommand` | SystemCommand | bursty | SystemCommandProfile | RELIABLE, WaitSet-handled |
| `Ship/ShipPosition` | ShipPosition | 10 Hz | ShipPositionProfile | keyed: 0 = INS, 1 = truth; key 0 consumed by HMI-UI (ship panel) |
| `TargetGen/TargetTruth` | TargetTruth | 50 Hz/target | TargetTruthProfile | keyed per target |

All keyed topics have a **bounded key space** (constant source ids, a
recycled track-id pool, a modulo command-id range): an ever-incrementing
key would register a new DDS instance per sample and grow writer/reader
memory without bound.

### DetectionProcessor loopback simplification

`Radar.DetectionProcessor` is a **deliberate architectural simplification** for the demo.
In a real phased-array radar the transmit and receive chains are physically separate:
the T/R modules fire a pulse, switch to receive microseconds later, and the digital
beamformer aggregates returns from all elements before handing them to the CFAR engine.

For this simulation, `DetectionProcessor` **both publishes and subscribes** to
`Radar/RawReturn` (the 1 kHz loopback shown in the diagram). The participant
**produces** the synthetic I/Q data — modeling the pulse-repetition-rate stream
that would come from the radar face — and then **consumes it back** to run CFAR
detection processing.

The realism is baked in via the `TargetGen/TargetTruth` subscription:
DetectionProcessor reads the ground-truth target positions, then synthesizes
range-bin I/Q samples with RCS-based amplitude, 1/r^4 range attenuation and a
Rayleigh noise floor. It applies the effective gain, width, pointing error,
and sidelobes received from `Radar.Beamformer` over
`Radar/BeamPatternStatus`, then spreads each return over ~3 bins as a
matched-filter response. The 1 kHz `RawReturn` stream therefore behaves like
genuine radar data rather than random numbers.

> **Production note:** a real system would not put raw I/Q on DDS at 1 kHz × 512 bins
> (~4 MB/s). The loopback exists here to stress-test the middleware and to make the
> full data flow visible in Connext Studio. A deployed architecture would use separate
> `Radar.Transmitter` and `Radar.Receiver` participants, with the raw data staying on
> a high-bandwidth internal fabric (PCIe, RDMA, or shared memory) rather than the
> DDS data bus.

### WaitSet vs. listener split

- **Listeners** (DDS receive threads): `RawReturn`, `BeamCommand`,
  `CalibrationStatus`, `TargetTruth`, `DetectionEvent`,
  `BeamPatternStatus` — high rate,
  lightweight callbacks that only cache or enqueue.
- **WaitSet** (dedicated thread): `SystemCommand` — lower rate, handled
  atomically and in order by `CommandHandler`.
- **Render thread**: never blocks on DDS; drains lock-free SPSC queues
  and mutex-protected stores from the `DataBus` at display rate. The
  `DataBus` is fed by the HMI-UI participant's listeners (tracks, blips,
  ship, health, beam pattern) and by component worker threads (A-scope trace,
  beam timeline). DDS threads never touch ImGui/OpenGL.

### Type system

All application types are `@appendable` (forward-compatible field
additions) and fully self-describing; with Connext 7.x the builtin
**TypeLookup Service** is enabled by default, so Connext Studio decodes
samples without the IDL file. Coordinate frames are documented in the IDL:
ship-relative polar for detections, ship-relative ENU for tracks/truth.

### Deliberate demo choices (not production patterns)

- Each radar component owns a **separate DomainParticipant** so Connext
  Studio's topology map shows `Radar.BeamScheduler`, `Radar.TrackManager`,
  etc. as individual nodes. A production system would use one participant
  with several publishers/subscribers.
- `RawReturn` at 1 kHz x 512 bins (~4 MB/s) exercises the bus for the demo;
  a real system would not put raw I/Q on DDS at this rate.
- QoS **variety is intentional** (BEST_EFFORT sensor paths vs RELIABLE
  command/track paths) so Studio's match analysis has something to show.

## Performance notes

- 60 FPS with 100+ tracks / 1000+ active blips: blip pooling (fixed ring,
  no per-frame allocation), preallocated polyline buffers, single GL
  texture upload per frame for the B-scope, SPSC handoff (no locks on the
  render path), delta-time animation everywhere.

## Connext Studio

See **[ConnextStudioDemo.md](ConnextStudioDemo.md)** for the webinar
play-by-play: prepare a steady-state Studio view, activate a scenario in the
radar workspace, return to observe the DDS changes, and reshape the live view
with Studio AI. See **[docs/CONNEXT_STUDIO.md](docs/CONNEXT_STUDIO.md)** for
the lower-level topology, QoS, TypeLookup, and mismatch reference.
