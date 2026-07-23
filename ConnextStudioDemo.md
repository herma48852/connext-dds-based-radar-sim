# Connext Studio Live Scenario Demo

This is the operator runbook for demonstrating AesaRadarSim through
[RTI Connext Studio](https://www.rti.com/developers/rti-labs/connext-studio).
It is organized around two workspaces and one repeatable presentation rhythm:

1. Prepare a steady-state DDS data or custom view in the **Studio View**
   workspace.
2. Switch to the **Radar Display** workspace and activate a scenario.
3. Switch back to the Studio View and observe the command and its downstream
   topic changes.
4. Ask Studio AI to reshape or extend the visualization while the system is
   live.
5. Restore the baseline before moving to the next scenario.

The intent is to show more than a radar animation. Each button becomes a live
demonstration of DDS discovery, typed data, keys and instances, QoS, command
delivery, and downstream state propagation.

## 1. Workspace layout

Keep these views open side by side or in two macOS desktop spaces:

- **Radar Display workspace** — the native `radar_app` window and the two
  terminals running `radar_app` and `target_gen`.
- **Studio View workspace** — a separate VS Code window with Connext Studio
  connected to the live DDS domain. An empty VS Code folder is sufficient.

The Studio View should already be subscribed to the relevant topic before a
scenario button is pressed. `Radar/SystemCommand` and `Radar/BeamCommand` use
volatile durability, so a view opened after the event may miss the command or
the beginning of the transition. `Radar/CalibrationStatus`,
`Radar/BeamPatternStatus`, and `Radar/TargetTrack` are transient-local and
retain their latest keyed state for late joiners.

## 2. Preflight

### 2.1 Eliminate stale processes

From the repository root, check for earlier demo processes:

```bash
ps aux | grep -E '[r]adar_app|[t]arget_gen'
```

Stop old instances normally before starting. Duplicate or stale DDS writers
can make a live demonstration misleading.

### 2.2 Build and launch on an isolated domain

This runbook uses domain 92. Use the same number for both applications and
Connext Studio.

```bash
# Run from the AesaRadarSim repository root.
cmake --build build -j8
```

Radar terminal:

```bash
source /Applications/rti_connext_dds-7.7.0/resource/scripts/rtisetenv_arm64Darwin23clang16.0.bash
./build/radar_app.app/Contents/MacOS/radar_app --domain 92
```

Target-generator terminal:

```bash
source /Applications/rti_connext_dds-7.7.0/resource/scripts/rtisetenv_arm64Darwin23clang16.0.bash
./build/target_gen --domain 92 --targets 16
```

The webinar scenario deliberately uses 16 targets: two repeats of the stable
eight-profile mix. Targets recycle past 120 km so the display remains busy.

### 2.3 Connect Connext Studio

In the Studio View workspace:

1. Connect Connext Studio to live DDS domain **92**.
2. Wait for the participant and topic map to stabilize.
3. Confirm these participants are visible:

   - `Radar.CommandConsole`
   - `Radar.CommandHandler`
   - `Radar.BeamScheduler`
   - `Radar.Beamformer`
   - `Radar.DetectionProcessor`
   - `Radar.TrackManager`
   - `Radar.CalibrationMonitor`
   - `Radar.ShipINS`
   - `Radar.HMI-UI`
   - `TargetGen.Generator`

4. Confirm that Studio decodes the application types through DDS TypeLookup;
   importing the IDL should not be necessary.

### 2.4 Establish the steady-state baseline

In the Radar Display workspace, press:

1. **ALL ONLINE**
2. **RESTORE ARRAY**
3. **SEARCH MODE**

Allow several seconds for the track picture to stabilize. In Studio, the
baseline should read approximately:

| Topic and field | Baseline |
|---|---|
| `Radar/CalibrationStatus.overall_status` | `ARRAY_NOMINAL` |
| `Radar/CalibrationStatus.failed_element_count` | `0` |
| `Radar/CalibrationStatus.rma_offline_mask` | `0` / `0x0000` |
| `Radar/BeamCommand.azimuth_deg` | repeating 0–360° sawtooth |
| `Radar/BeamCommand.priority` | `3` |
| `Radar/TargetTrack` | several live keyed instances |

## 3. Standard play-by-play

Use the same narration for each scenario:

1. **Prepare:** “First I am subscribing to the steady-state data before I
   issue any command.”
2. **Orient:** identify the publisher, topic, type, key, update rate, and QoS
   relevant to the view.
3. **Activate:** switch to the radar and press exactly one scenario control.
4. **Observe:** return to Studio and point out the `SystemCommand` sample
   first, then the downstream topic response.
5. **Adapt:** use Studio AI to change the view—add a field, filter an instance,
   annotate the command time, reshape a sequence, or explain a QoS choice.
6. **Restore:** return the radar to its baseline and confirm that the Studio
   view returns as expected.

Typical response times are:

| Path | Expected response |
|---|---|
| UI → `Radar/SystemCommand` | immediate burst |
| Sector command → `Radar/BeamCommand` | next 10 ms scheduler cycle |
| Array command → `Radar/CalibrationStatus` | state-change publish, normally within 20 ms |
| `CalibrationStatus` → `Radar/BeamPatternStatus` | next 20 Hz beamformer update, within 50 ms |
| Reset command → track disposal | next 100 ms tracker cycle |
| Track reacquisition after reset | several beam revisits |

## 4. Scenario 1 — search mode to sector scan

### Prepare the Studio View

Open or generate:

- A sample log for `Radar/SystemCommand`.
- A time chart for `Radar/BeamCommand.azimuth_deg`.
- Optional traces for `elevation_deg` and `priority`.
- Filter `Radar/BeamCommand` to `scheduler_id = 0`.

Suggested AI prompt:

> Subscribe to `Radar/SystemCommand` and `Radar/BeamCommand` on domain 92.
> For `scheduler_id = 0`, create a scrolling time chart of azimuth and
> elevation, put priority in a second panel, and mark each SystemCommand on
> the common time axis. Use a 15-second window.

In steady search, `azimuth_deg` repeatedly sweeps through 0–360°,
`elevation_deg` cycles among 3°, 14°, and 25°, and priority is 3.

### Activate

Switch to the Radar Display workspace and press **SECTOR SCAN**.

### Observe

Return to Studio and show:

1. `Radar/SystemCommand.command_type = CMD_SET_SECTOR`.
2. `sector_center_deg = 90` and `sector_width_deg = 60`.
3. `Radar/BeamCommand.azimuth_deg` now bounces between 60° and 120°.
4. `Radar/BeamCommand.priority` changes from 3 to 2.

Do not use `BeamCommand.mode` to distinguish these two views; both are emitted
as `BEAM_MODE_SEARCH` in the current model. The azimuth bounds and priority
are the observable downstream evidence of sector operation.

### Adapt the view with AI

Follow-up prompts:

> Shade the commanded 60–120 degree sector and highlight each reversal of
> scan direction.

> Add a calculated indicator that reads FULL SEARCH when the azimuth span is
> approximately 360 degrees and SECTOR when it remains between 60 and 120.

### Restore

Press **SEARCH MODE**. Studio should show `CMD_SET_MODE` with parameters
`"search"`, followed by a return to the 0–360° sweep and priority 3.

## 5. Scenario 2 — degrade and restore the array

### Prepare the Studio View

Subscribe to the single keyed instance `array_id = 0` on
`Radar/CalibrationStatus`. This topic publishes on each state change plus a
1 Hz heartbeat and contains both the scalar health summary and the full face
data:

- `overall_status`
- `failed_element_count`
- `rma_offline_mask`
- `temperature_c`
- `element_drift_db[1024]`, a 32×32 row-major face

Suggested AI prompt:

> Subscribe to `Radar/CalibrationStatus` for `array_id = 0`. Create a time
> chart of failed element count and overall status. Render
> `element_drift_db` as a 32 by 32 row-major heatmap: green above -0.45 dB,
> amber from -1 to -0.45 dB, red below -1 dB, and dark red below -20 dB.
> Show the RMA offline mask in hexadecimal and as a 4 by 4 tile overlay.

The steady state should be nominal, with zero failed elements and mask
`0x0000`.

### Activate

Switch to the Radar Display workspace and press **DEGRADE ARRAY**.

### Observe

Return to Studio and show the two-stage data flow:

1. `Radar/SystemCommand.command_type = CMD_DEGRADE_ARRAY`.
2. On the prompt calibration update, `overall_status` changes from
   `ARRAY_NOMINAL` to `ARRAY_DEGRADED`.
3. `failed_element_count` jumps from 0 to 128 in the current deterministic
   pattern.
4. The same 128 positions in `element_drift_db` become hard failures around
   -6 dB or below.
5. `rma_offline_mask` remains `0x0000` because no complete RMA was taken out
   of service.

The diagonal-looking red pattern is a stable demo pattern, not propagation of
physical damage. This scenario currently changes calibration and health data;
it does not reduce DetectionProcessor target gain. Whole-RMA removal, covered
next, has the modeled signal-path effect.

### Adapt the view with AI

Follow-up prompts:

> Add an event marker where failed_element_count first becomes nonzero and
> calculate failed elements as a percentage of 1024.

> Replace the heatmap with sixteen small 8 by 8 panels, one per RMA, and show
> the failed-element count within each panel.

> Compare the last nominal sample with the current degraded sample and list
> only element indices whose drift crossed below -1 dB.

### Restore

Press **RESTORE ARRAY**. Show `CMD_RESTORE_ARRAY`, then the next calibration
sample returning to `ARRAY_NOMINAL` and zero failed elements.

RESTORE ARRAY clears the sparse degraded-element scenario. It does not clear
whole RMAs that were separately taken offline; use **ALL ONLINE** for those.

## 6. Scenario 3 — RMA offline and ALL ONLINE

### Prepare the Studio View

Reuse the `Radar/CalibrationStatus` dashboard. Add:

- A time chart of `rma_offline_mask` and `failed_element_count`.
- A calculated `popcount(rma_offline_mask & 0xFFFF)` if transformations are
  available.
- The 32×32 drift heatmap with 4×4 RMA boundaries.
- `Radar/BeamPatternStatus` scalar charts for `gain_loss_db`,
  `beamwidth_3db_deg`, `peak_sidelobe_level_db`, and
  `boresight_error_deg`.
- A line view of its 181-sample `azimuth_pattern_db` sequence.
- Optionally, a chart of `Radar/DetectionEvent.snr_db` or `amplitude` to
  illustrate the downstream signal effect as more RMAs are removed.

Suggested AI prompt:

> Visualize `rma_offline_mask` as sixteen labeled RMA indicators in a 4 by 4
> row-major grid. Overlay those boundaries on the 32 by 32 element drift
> heatmap. Add time charts for offline RMA count and failed element count,
> synchronized with Radar/SystemCommand events.

> Correlate SystemCommand and CalibrationStatus mask transitions with
> BeamPatternStatus gain loss, 3 dB beamwidth, boresight error, and peak
> sidelobe level. Plot the azimuth pattern before and after each outage.

### Activate

In the Radar Display workspace, click RMA 3—the top-right block when numbered
row-major from 0—to take it offline.

### Observe

Return to Studio and show:

1. `Radar/SystemCommand.command_type = CMD_RMA_OFFLINE` with parameters `"3"`.
2. On the prompt calibration sample, `rma_offline_mask` changes from `0x0000`
   to `0x0008`.
3. `failed_element_count` increases by 64.
4. The corresponding 8×8 region of `element_drift_db` becomes -60 dB.
5. `Radar/BeamPatternStatus.rma_offline_mask` changes in the 20 Hz stream;
   `gain_loss_db`, `beamwidth_3db_deg`, `peak_sidelobe_level_db`, and
   `boresight_error_deg` describe the response.
6. The B-scope automatically shows commanded/effective beam centers, the
   widened main lobe, dominant sidelobes, and the compact degraded readout.
7. Detection amplitude/SNR can decline as active aperture is removed. A
   strong contact can occasionally appear through a dominant sidelobe at a
   displaced dwell azimuth.

The mask is the compact 16-RMA state. The 1,024-value drift sequence is the
per-element face that supports a heatmap. The 181-value pattern sequence is a
normalized -45 to +45 degree azimuth cut around the commanded beam.

For a more theatrical second step, also click RMA 7. RMAs 3 and 7 form an
adjacent pair on the right edge of the logical face, making the gain,
boresight, and sidelobe changes easier to see while preserving most of the
aperture.

### Adapt the view with AI

Follow-up prompts:

> Decode every transition of rma_offline_mask into the RMA indices that
> changed and annotate whether each one went offline or online.

> Correlate offline RMA count with median DetectionEvent SNR in ten-second
> windows. Use separate panels and align them by source timestamp.

> Focus the heatmap on RMA 3 and display the 64 raw drift values next to it.

> Compare two BeamPatternStatus samples with the same offline-RMA count but
> different masks. Quantify how RMA position changes the azimuth pattern.

### Restore

Press **ALL ONLINE**. Studio should show `CMD_RMA_ONLINE` with parameters
`"all"`, followed by mask `0x0000`, the failed count dropping by 64 per RMA
that was offline, the pattern metrics returning to nominal, the B-scope
overlay disappearing, and the dark blocks returning to nominal drift.

## 7. Scenario 4 — reset and reacquire tracks

### Prepare the Studio View

Subscribe to `Radar/TargetTrack` and retain instance lifecycle information.
The topic is keyed by `track_id`, publishes active tracks at 10 Hz, and uses
reliable transient-local delivery.

Suggested AI prompt:

> Subscribe to `Radar/TargetTrack` and `Radar/SystemCommand`. Create a live
> count of ALIVE track instances, a table of track ID, classification,
> quality, and range, and a timeline of instance disposals and new track
> births. Mark RESET commands on the timeline.

Let the view reach a steady state with several tracks.

### Activate

Switch to the Radar Display workspace and press **RESET TRACKS**.

### Observe

Return to Studio and show:

1. `Radar/SystemCommand.command_type = CMD_RESET`.
2. Existing keyed `Radar/TargetTrack` instances are disposed on the next
   tracker cycle.
3. The old live-instance count drops; depending on view sampling, zero may be
   brief because detection processing continues during reset.
4. New track instances appear as detections are correlated over subsequent
   beam revisits.

This is a strong DDS lifecycle demonstration: reset does not merely clear a
local table; the writer disposes each keyed instance so remote subscribers can
observe the transition.

### Adapt the view with AI

Follow-up prompts:

> Measure elapsed time from the RESET command to the first new track and to
> recovery of the pre-reset live-track count.

> Color track births by classification and show quality growth for each new
> track after reset.

RESET also returns the scheduler to search mode, so a synchronized
`BeamCommand.azimuth_deg` chart can show the full scan resuming.

## 8. Scenario 5 — self-test command-path demonstration

### Prepare the Studio View

Open `Radar/SystemCommand` as a sample/event log and show its reliable,
keep-last-depth-5 QoS.

Suggested AI prompt:

> Create a command audit view for Radar/SystemCommand showing source
> timestamp, command ID, symbolic command type, parameters, and priority.
> Highlight SELF_TEST commands.

### Activate and observe

Press **SELF TEST**, then return to Studio and show
`command_type = CMD_SELF_TEST`.

Current limitation: the CommandHandler records the internal self-test request,
but no component consumes it and no DDS self-test result topic is published.
Use this scenario only to demonstrate the reliable command path. Do not promise
a downstream state transition. A future `Radar/SelfTestStatus` topic would
make this a richer end-to-end scenario.

## 9. Use Studio AI as part of the story

Avoid presenting only prebuilt dashboards. Start each scenario with a simple
steady-state view, then change it conversationally after the scenario fires.
This demonstrates that the operator can ask new questions of a running DDS
system without changing or restarting the applications.

A useful prompt pattern is:

> Using domain 92, subscribe to **[topic]**, filter to **[key]**, visualize
> **[fields]** over **[time range]**, calculate **[derived value]**, and
> annotate samples where **[event condition]** occurs.

Examples of dynamic follow-ups:

- “Add the SystemCommand that caused this transition as a vertical marker.”
- “Change the time window from 15 seconds to two minutes without losing the
  current subscription.”
- “Split this view by keyed instance and show only instances updated in the
  last five seconds.”
- “Explain why this DataReader matches the DataWriter and summarize their
  reliability, durability, history, and deadline QoS.”
- “Turn this numeric RMA mask into a labeled 4×4 status view.”
- “Reshape this 1,024-element sequence into a 32×32 heatmap.”
- “Compare the five seconds before and after the command and summarize which
  DDS fields changed.”

If a requested custom chart is unavailable in the installed Studio build,
fall back to a scalar time chart plus the live sample inspector. The best
scalar signals are `failed_element_count`, `rma_offline_mask`, beam azimuth,
beam priority, and live `TargetTrack` instance count.

## 10. Scenario-to-topic quick reference

| Radar control | Command | Primary downstream topic | Fields to watch | Restore |
|---|---|---|---|---|
| SEARCH MODE | `CMD_SET_MODE` | `Radar/BeamCommand` | azimuth 0–360°, priority 3 | — |
| SECTOR SCAN | `CMD_SET_SECTOR` | `Radar/BeamCommand` | azimuth 60–120°, priority 2 | SEARCH MODE |
| DEGRADE ARRAY | `CMD_DEGRADE_ARRAY` | `Radar/CalibrationStatus` | status, failed count, drift sequence | RESTORE ARRAY |
| RESTORE ARRAY | `CMD_RESTORE_ARRAY` | `Radar/CalibrationStatus` | return to nominal sparse-element state | — |
| RMA block click | `CMD_RMA_OFFLINE/ONLINE` | `Radar/CalibrationStatus`, `Radar/BeamPatternStatus`, `Radar/DetectionEvent` | mask, drift block, gain loss, beamwidth, sidelobes, SNR | click again or ALL ONLINE |
| ALL ONLINE | `CMD_RMA_ONLINE`, `"all"` | `Radar/CalibrationStatus` | mask returns to zero | — |
| RESET TRACKS | `CMD_RESET` | `Radar/TargetTrack`, `Radar/BeamCommand` | disposals, live count, reacquisition, search scan | automatic reacquisition |
| SELF TEST | `CMD_SELF_TEST` | command only today | `Radar/SystemCommand` event | not applicable |

## 11. Troubleshooting during a webinar

- **No participants or topics:** confirm Studio and both applications use
  domain 92.
- **Duplicate participants or impossible data:** check for stale
  `radar_app` or `target_gen` processes.
- **Command missing from Studio:** subscribe to volatile
  `Radar/SystemCommand` before pressing the button, then repeat the command.
- **Array appears unchanged:** verify that `Radar.CalibrationMonitor` has a
  matched `CalibrationStatus` reader at both `Radar.Beamformer` and
  `Radar.HMI-UI`; state changes should publish within about 20 ms.
- **DEGRADE does not change the RMA mask:** expected. It changes sparse
  element drift and failed count, not whole-RMA state.
- **RESTORE leaves a dark RMA:** expected. Use ALL ONLINE to clear the RMA
  mask.
- **Tracks return after RESET:** expected. Reset disposes current instances;
  continuing detections initiate new tracks.
- **A-scope still shows activity with all RMAs offline:** the current model
  continues to display simulated receiver noise while target returns are
  reduced to a 1% gain floor.

For the lower-level topology, QoS, mismatch, and TypeLookup reference, see
[`docs/CONNEXT_STUDIO.md`](docs/CONNEXT_STUDIO.md).
