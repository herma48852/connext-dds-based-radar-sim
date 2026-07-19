# AesaRadarSim — Continuation TODO

State as of 2026-07-20. Build is green; both apps run. One open crash
investigation. Read this first when resuming with a fresh context.

---

## 1. OPEN ISSUE — SIGSEGV after ~1 minute of runtime

### Symptom

`./build/radar_app` runs fine for 60+ seconds (UI fully live, sawtooth
sweep, tracks, blips), then SIGSEGV. The crash handler in
`src/radar_app/main.cpp` now prints a backtrace automatically — captured:

```
2  AppKit  -[NSView effectiveAppearance] + 212
6  AppKit  -[NSTitlebarView updateMaterialForSection:atIndex:]
8  AppKit  -[NSTitlebarContainerView _forceDisplayOfDividers:...]
9  AppKit  _startForceShadowContentDividersTimer_block_invoke
10 Foundation __NSFireTimer
...
23 radar_app _glfwPollEventsCocoa + 88
24 radar_app radar::ui::UiApp::run() + 200
```

### Analysis so far

- Crash is on the **main thread, inside AppKit** (a Foundation timer
  walking the window titlebar views hits a dangling pointer).
- **NOT a DDS crash.** No Connext frames in the stack.
- Verified: **zero GLFW/OpenGL/ImGui calls outside `src/radar_app/ui/`** —
  no component thread touches the window system.
- Therefore prime suspect: **heap corruption in our own code** (an
  overflow trashes AppKit objects; the titlebar timer is just where it
  gets detected). Second suspect: a GLFW/macOS titlebar interaction, but
  nothing in the code mutates the window after creation.
- History note: the earlier "silent SIGSEGV:11" and "SIGBUS:10 after
  sustained runtime" were very likely this same bug all along. The DDS
  instance-leak fix (below) removed unbounded instance growth but did
  **not** fix this crash.

### Code audit progress (render path, most likely corruptor)

- `src/radar_app/ui/UiApp.cpp` — **read, clean.** Standard ImGui loop,
  no per-frame window mutation, no raw buffers.
- `src/radar_app/ui/PpiView.cpp` — **read, nothing conclusive.** Blip
  deque bounded at 2048; track trails pruned against live tracks.
- **Still to audit (in priority order):**
  1. `src/radar_app/ui/BScopeView.cpp` — GL texture splat math; classic
     overflow site (check row/column bounds when azimuth wraps or
     `range_m > range_max`; check texture size vs. buffer size).
  2. `src/radar_app/ui/AScopeView.cpp` — polyline buffer sizing.
  3. `src/radar_app/ui/Panels.cpp` + `Theme.hpp`.
  4. `src/common/SpscQueue.hpp` — lock-free index math (producer pushes,
     consumer drains; already fixed one producer-side pop race).
  5. `src/radar_app/CommandConsole.hpp`, `components/CommandHandler.cpp`.

### Fastest path to the answer: AddressSanitizer

One ASan run should name the exact file/line of the overflow:

```bash
cd /Users/fherman/rti_workspace/7.7.0/my_examples/AesaRadarSim
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
cmake --build build-asan -j8
./build-asan/radar_app    # plus ./build/target_gen --targets 8
```

Paste the ASan report back. (TSan is the fallback for a data race, but
expect noise from Connext internals.)

---

## 2. Recently completed (don't redo)

- **HMI-UI is now a real DomainParticipant** (`Radar.HMI-UI`,
  `src/radar_app/components/HmiUi.{hpp,cpp}`): subscribes TargetTrack,
  DetectionEvent, ShipPosition (key 0), CalibrationStatus. Every panel is
  DDS-fed; **no dangling publishers**. radar_app now has **8
  participants**. Producers no longer feed the UI in-process
  (TrackManager/DetectionProcessor/CalibrationMonitor cleaned).
  Ship-for-components, A-scope trace, beam timeline stay in-process
  deliberately (documented in README).
- **Dispose pattern:** TrackManager registers instance handles at track
  creation, `dispose_instance` on coast-drop and on reset; HmiUi's
  TrackListener recovers the key and drops the track immediately.
  Age-out backstop: 6 s.
- **RTI 7.7 API facts learned the hard way:** `SampleInfo` has **no**
  `instance_state()`; `DataReader::key_value` is the **two-arg**
  out-param form (`reader.key_value(key, handle)`).
- **Instance-leak fix (the old SIGBUS suspect):** every keyed topic now
  has a bounded key space — `BeamCommand.scheduler_id`,
  `RawReturn.array_id`, `DetectionEvent.sensor_id` are constant 0;
  `TargetTrack.track_id` recycled pool (1000+, ≤256, free-id scan);
  `SystemCommand.command_id` = 100 + (n % 128).
- **Crash handler** installed in `radar_app/main.cpp` (SIGSEGV/SIGBUS/
  SIGABRT → backtrace to stderr; POSIX only).
- **Font-scale clamp** in UiApp.cpp (guards 0/NaN content scale).
- Docs fully synced: README, `docs/dds_architecture.{svg,png}`,
  `docs/CONNEXT_STUDIO.md` (topology table includes HMI-UI). K2.6's
  loopback section kept; K2.6's invented subscriptions corrected, then
  made real by the HMI-UI work.

---

## 3. Environment & hard-won gotchas (read before touching anything)

- Repo: `github.com/herma48852/connext-dds-based-radar-sim` (private;
  unreachable from the assistant sandbox — sync via file upload/rsync).
- Build host: Mac Mini Apple Silicon, Connext **7.7.0 LTS**, arch
  `arm64Darwin23clang16.0`; source `rtisetenv_*.bash` before running.
- **rtiddsgen C++11 modern mapping:** structs → public **data members**
  (no accessor methods!), enums → `enum class`, sequences → `std::vector`.
- **QoS loading:** plain filesystem paths only (single-slash `file:` URI
  aborts). Precedence: `$RADAR_QOS_FILE` → `./qos/radar_qos.xml` →
  `../qos/radar_qos.xml`. QoS is runtime-loaded — no rebuild after edits.
- **UDPv4-only** transport masks (macOS sysv shmem limits, RTI KB
  osx510; 8 participants exhaust 32 segments). Deferred: persistent
  sysctl/LaunchDaemon config to re-enable SHMEM — user said UDPv4-only
  is fine for now.
- **CFAR calibration:** `kCfarThreshold = 0.26`, `kSignalScale = 2.0e8`
  in DetectionProcessor.hpp; AScopeView threshold mirrors at 0.26.
  (Old values 0.09/2e7 caused ~30k false detections/s.)
- **WaitSet:** standard `dds::core::cond::WaitSet` from
  `<dds/core/cond/WaitSet.hpp>` (the `rti/...` path does not exist).
- **File-edit tooling flakiness:** edits intermittently report success
  but don't persist (especially several edits to the same file in one
  batch). Apply edits **one at a time per file** and **verify by
  re-reading** before moving on.
- Sync direction: assistant sandbox → repo. After syncing from the
  sandbox: `rsync -a --delete --exclude='build' --exclude='.git'
  <sandbox tree>/ <repo>/` then `cmake --build build -j8`.

## 4. Webinar prep (after the crash is fixed)

- [ ] Rehearse `docs/CONNEXT_STUDIO.md` runbook end-to-end (topology
      map with 8+1 participants, TypeLookup decode, QoS match analysis,
      the three injection scenarios).
- [ ] Show dispose in action: track reset → instances vanish live in
      Studio (HMI-UI dispose path).
- [ ] Verify `--inject-qos-mismatch` and `--inject-type-mismatch` still
      behave with the re-keyed IDL (RogueReader/RogueWriter paths).
- [ ] Optional: persistent macOS sysv shmem config (KB osx510) if SHMEM
      is wanted back for the demo.
