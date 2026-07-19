# AesaRadarSim — Continuation TODO

State as of 2026-07-20 (evening). Build is green; both apps run. One open
crash investigation. Read this first when resuming with a fresh context.

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

### FULL code audit COMPLETE (2026-07-20, K3) — our code is clean

Every .cpp/.hpp in the repo was read line-by-line with heap corruption as
the search target. **No out-of-bounds write exists anywhere.** Verdicts:

- `ui/BScopeView.cpp` — splat/flip/LUT indices provably bounded
  (mod/clamp after every cast). Texture size == buffer size. CLEAN.
- `ui/AScopeView.cpp` — phosphor/points sized from trace length. CLEAN.
- `ui/Panels.cpp` + `ui/Theme.hpp` — bounded ImPlot arrays, no raw math.
  CLEAN.
- `common/SpscQueue.hpp` — textbook SPSC: head written only by producer,
  tail only by consumer, correct acquire/release. **Producer uniqueness
  verified per queue**: `detection_blips` ← HmiUi listener only,
  `beam_commands` ← BeamScheduler only, `CommandConsole::queue_` ←
  render thread only. CLEAN.
- `CommandConsole.hpp` / `components/CommandHandler.cpp` — CLEAN.
- `ui/PpiView.cpp` / `ui/UiApp.cpp` — re-audited; blip deque ≤2048,
  trails ≤10 and pruned, beam history ≤240. CLEAN.
- `DataBus.hpp` — trace double-buffer swapped under mutex; UI receives
  its own copy of every store. CLEAN.
- Components (DetectionProcessor, TrackManager, HmiUi, BeamScheduler,
  CalibrationMonitor, ShipSimulator, CommandHandler), both mains,
  target_gen, IDL↔code constants, QoS profile names — CLEAN.

### Hardening fixes applied this session (real defects, small blast radius)

1. `DetectionProcessor::on_raw_return` indexed `iq_samples[2*i]` trusting
   the sender's `range_bin_count` over the **actual received sequence
   length** — OOB read if any foreign/malformed publisher writes
   RawReturn. Now `n` is also bounded by `iq_samples.size()/2`.
2. `BScopeView::splat` — NaN/Inf blip fields made the `(int)` casts UB.
   Now: `isfinite` reject, modulo in `long`, clamp in `double`.
3. `SimClock::started_` — plain bool cross-thread flag; now
   `std::atomic<bool>` with acquire/release.
4. `UiApp::run` — layout could go negative for extreme content scales;
   `panel_h` now clamped to `[100, 90% H]`.

### KEY FACT (2026-07-20 PM): every crash so far is radar_app ALONE

**target_gen has never been run.** All crashes (60 s AppKit SIGSEGV,
earlier silent SIGSEGV/SIGBUS, 20 s SIGBUS) happened with radar_app as
the only process — the bug is fully self-contained; no second terminal
or external publisher is needed to reproduce it.

The dispose chain runs on empty space via CFAR false alarms:

- Pfa ≈ 1.4e-6/bin (kCfarThreshold 0.26, Rayleigh σ 0.05) × 512 bins ×
  1 kHz ≈ one false blip every few seconds, BY DESIGN.
- Each false blip → tentative track (registered DDS instance).
- No correlating second hit → coasts out after exactly 5 s →
  dispose_instance fires. Rate ≈ 10–15 disposes/min from t ≈ 10 s on.
- First dispose lands squarely in the observed 20–60 s crash windows;
  crash-time variance matches the stochastic false-alarm process
  (mt19937 seeded from random_device → different every run).

### 2026-07-20 PM: second crash capture — same bug, DDS-layer victim

Patched build died at **20 s** with **SIGBUS:10**, this time on the
TrackManager thread, inside Connext's writer-history buffer pool:

```
2  REDAFastBufferPoolSet_getBuffer + 152      <- wild free-list pointer
5  WriterHistorySessionManager_getNewSample
9  WriterHistoryMemoryPlugin_addSample
10 PRESWriterHistoryDriver_addDispose
12 PRESPsWriter_disposeInternal
14 rti::pub::UntypedDataWriterView::dispose_instance
15 radar::app::TrackManager::update_loop + 788
```

- SIGBUS:10 was in the crash history BEFORE any patches (see "History
  note" above) — same underlying bug, different innocent bystander.
- The corruptor tripped inside Connext during
  `writer_.dispose_instance(handle)` — the newest, least-exercised DDS
  code in the system (§4's "show dispose in action" was never run).
- Timing fits: the first dispose fires when the first tentative track
  coasts out (~15–30 s in). Plausible that earlier 60 s AppKit crashes
  were downstream victims of the same ~20 s dispose event.
- No matching RTI known issue for this stack (checked 7.x release
  notes, CORE crash-fix lists).

### E1/E2 RESULTS (2026-07-20 eve) — VERDICT: corruptor is in the UI layer

- **E1 (`--headless`, dispose ACTIVE): ran 11 min CLEAN.** The whole DDS
  layer — 8 participants, 1 kHz loopback, TrackManager, dispose path
  firing on false-alarm tracks — is EXONERATED. **H1 (dispose) is dead.**
- **E2 (`--no-dispose`, windowed): crashed in 12 s**, main thread,
  `glfwPollEvents` → `nextEventMatchingMask:` → `NSEvent
  _initWithCGEvent:` → `_findWindowUsingCache:` → `-[NSRecursiveLock
  unlock]` on garbage. Removing dispose changed nothing.
- Both windowed crashes are the same animal: `glfwPollEvents` → AppKit
  window bookkeeping (titlebar view list / NSApplication window cache)
  walked into a dangling pointer. **The bug only manifests with the
  GLFW/ImGui/GL UI running.**
- Remaining suspects, in order: (a) our UI glue + ImGui 1.90.5 + ImPlot
  0.16 + GLFW 3.4 — ALL built from source, hence ASan-coverable;
  (b) Apple's GL-on-Metal shim (per-frame 360x256 texture re-upload);
  (c) macOS AppKit/GLFW interaction bug. Checked GLFW changelog since
  3.4: no Cocoa crash fixes (post-3.4 work is Wayland/X11) — bump stays
  as a low-expectation last resort.

### Experiment ladder (final form; in order)

1. **ASan, windowed, solo** — covers our UI code + ImGui + ImPlot +
   GLFW entirely (all FetchContent/source-built). **CRITICAL: GLFW is C
   — pass the sanitizer in CMAKE_C_FLAGS too or GLFW stays
   uninstrumented:**
   ```bash
   cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
     -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
     -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
     -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
   cmake --build build-asan -j8
   ASAN_OPTIONS=halt_on_error=1:abort_on_error=1 ./build-asan/radar_app
   ```
   Repro takes <60 s. Paste the first ASan report verbatim.
   (Connext dylibs are uninstrumented — fine; they're exonerated by E1.)
2. **No rebuild:** `NSZombieEnabled=YES ./build/radar_app` — a dangling
   NSView/NSWindow gets a console log naming the freed object's class
   and address. Then `MallocScribble=1 MallocPreScribble=1
   ./build/radar_app` — poisoned freed memory crashes the FIRST reader
   (0x55... pattern), closer to the cause.
3. **GL-on-Metal theory:** throttle the B-scope texture upload to every
   4th frame (patch ready on request). Crash gone → driver path; keep
   the throttle as the fix (15 Hz is invisible on a 4 s phosphor).
4. **Connext debug libs** (`cmake -B build-dbg
   -DCMAKE_BUILD_TYPE=Debug`) — retained only as a formality; E1 makes
   Connext involvement unlikely.
5. **GLFW master bump** (`GIT_TAG 3.4` → `master` in CMakeLists) —
   cheap, but no relevant Cocoa fixes per the changelog.
6. TSan last (noisy with Connext internals, as before).

Report back: ASan report (or "ASan silent + zombie/scribble output").

---

## 2. Recently completed (don't redo)

- **Full heap-corruption audit of the entire codebase** (2026-07-20):
  every file read; no OOB write found. Four hardening fixes applied
  (RawReturn seq-length bound, B-scope NaN-safe binning, atomic
  SimClock flag, clamped UI layout). Details in §1.
- **`radar_app --headless`** soak mode added (components only, no UI) —
  the bisect tool for the crash investigation.
- **`radar_app --no-dispose`** toggle added (`bus_.dispose_enabled`;
  guards both dispose sites in TrackManager) — isolates the dispose
  path, the prime suspect after the 20 s SIGBUS in Connext's
  writer-history pool.
- **UI layout fix** (2026-07-20 eve): the bottom-strip formula
  multiplied by an ALREADY-scaled WindowPadding AND by FontGlobalScale,
  so on Retina it reserved 1110pt of an 1100pt window — PPI/A/B scopes
  were crushed to ~100px. Now `panel_h = clamp(240 × ui_scale, 27% H,
  50% H)` (scopes get ≥50–56%); PPI geometry uses the content area
  (title bar excluded) with the HDG/SPD/RNG readout on a reserved top
  line; health/ship panels widened (15%→18%, text was clipped);
  redundant in-content "SCENARIOS" title removed so 7 buttons fit.
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
