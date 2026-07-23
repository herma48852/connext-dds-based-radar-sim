# AesaRadarSim — Continuation TODO

## 0. HANDOFF — read this first (2026-07-20 late, K3)

Written for a fresh agent session (e.g. Kimi Code). Everything below the
line is the full case log; this section is the executive summary.

### 2026-07-21 — RESOLVED: SCENARIOS and ALL ONLINE buttons

**Symptom:** all six SCENARIOS buttons and ARRAY FACE's `ALL ONLINE`
button were dead, while the manually hit-tested RMA blocks worked. Mouse
hover/press and the DDS command pipeline were healthy, but ImGui cleared a
button's ActiveID one frame after press and before release.

**Root cause captured under LLDB:** `AScopeView::render()` included live
azimuth/elevation values in the string passed to `ImGui::Begin()`. ImGui uses
the full window name as its ID, so every beam-value change created a newly
appearing A-scope window. On the next frame its `Begin()` called
`FocusWindow()`, which correctly cleared the ActiveID owned by the SCENARIOS
or ARRAY FACE window. Exact stack:

`ClearActiveID → FocusWindow (imgui.cpp:7235) → Begin (imgui.cpp:6958) → AScopeView::render (AScopeView.cpp:24) → UiApp::run`.

**Fix:** append the stable hidden ID `###A_SCOPE` to the dynamic A-scope
heading. The az/el readout still changes visibly, but ImGui now reuses one
window and no longer steals focus every frame. This also stops accumulation
of one ImGui window record per distinct heading. The temporary
`imgui_internal.h`/ActiveID/click diagnostics were removed; the keeper
`scenario_button()` active-state highlight and `radar_mode`/`degraded` inputs
remain.

**Verified:** full build green; `tracker_replay 300` unchanged (1761
detections, 198 births, 211 deaths). Rebuilt bundle on isolated domain 92:
`SECTOR SCAN` produced `[CommandHandler] command=1`, and `ALL ONLINE`
produced `[CommandHandler] command=7 params="all"`.

**Regression coverage added (2026-07-21):** CTest now runs three headless,
assertion-based tests in ~2 seconds. `ui_controls_smoke` drives production
ImGui widgets with real press/hold/release frames and covers all six SCENARIOS
buttons, RMA offline/online, ALL ONLINE, the dynamic A-scope focus regression,
and stable ImGui window count. `target_scenario_regression` accelerates a
30-minute run of the production 16-target webinar scenario, including the
repeated eight-profile mix and periodic 120 km respawns. The existing
`tracker_replay` is registered as a golden regression (1761/198/211 plus
bounded track IDs) while its normal manual diagnostic mode is unchanged.
Run all with `ctest --test-dir build --output-on-failure`; all 3 pass. No test
opens a display or creates a DDS participant.

**Geometry gotcha retained:** the OS may resize the app window from its
requested 1800x1100 dimensions, and the user may resize it further. Never
trust frame-1 geometry when diagnosing hit testing.

### 2026-07-20 (midday, K4) — heartbeat verdict: pipeline healthy; orphan incident

- **Heartbeat runs (fresh 12:07 build):** tracking chain verified end-to-end
  TWICE windowed + target_gen (dets_in climbing past 600, published 2–5,
  `[HmiUi] hb tracks` following 1:1) and once solo. **Empty-table incident
  NOT reproduced** — treat as one-off unless it recurs; heartbeat prints
  stay in the build to catch it.
- **ORPHAN PROCESS POISONED DOMAIN 0:** a leftover `target_gen --targets 8`
  from the 3 AM Stage-0 session (PID 98101, pre-rebuild binary) ran ~9 h
  undetected. With it alive, HMI-UI's ShipPosition reader rejected EVERY
  sample from ALL THREE ShipPosition writers (~28 FATAL/s):
  `InfrastructurePSM.c:70 RTI0x3000035 !precondition: unexpected
  instruction index injected into non-mutable program` → "deserialize
  sample error in topic 'Ship/ShipPosition'". Solo radar_app: ZERO errors.
  Orphan killed + fresh target_gen rerun: ZERO errors, ~110 s clean.
  Mechanism (unproven, RTI material): a stale/duplicate writer's
  TypeObject poisons the reader's XCDR coercion program for the whole
  type — Connext's error lines then name INNOCENT writers too. The
  poisoned run also SIGTRAP'd at 31 s after ~900 such FATALs — plausibly
  downstream of the storm, not a tenth victim.
- **RULE for every runbook:** `ps aux | grep -E 'radar_app|target_gen'`
  before ANY experiment and kill leftovers — a zombie participant
  falsifies results. Note the bundle .app keeps no stdout: run the binary
  with stdout redirected to a log or the heartbeats/FATALs are invisible.
- Crash investigation status UNCHANGED: the orphan postdates all 9
  victims (started 3 AM, after the last crash session) — it explains none
  of them. Ladder stands: TSan next; `build-tsan/` is configured with
  `-fsanitize=thread` in C/CXX/OBJCXX + linker flags.

### 2026-07-20 (eve, K5) — classification trilogy FIXED (the "only UNK" complaint)

User saw the pane degrade to "only an occasional UNK". rtiddsspy on the
live domain showed rows existed but were UNK-dominated, then SURF-dominated
after a first fix. Three real defects, all in the classification path:

1. **Stuck-at-birth classification (the big one):** `classify()` ran only
   while `classification == UNK`, at hits≥3 — BEFORE velocity seeds
   (cross_hits≥2). Fast movers classified with v=0 and were stuck
   forever: rtiddsspy caught 240–260 m/s tracks reading SURF. Now
   re-evaluated EVERY cycle at hits≥3.
2. **Impossible SURF rule:** old `speed<30 && z<50` — z is bar-quantized
   (ship reads ~2.6 km at 50 km), so SURF could never fire and the ship
   read UNK forever. New rule from the deck-bar physics: surface contacts
   only illuminate on the 3° bar (higher bars start at 8.5°), so z ≈
   R·sin3° ≈ 0.05R; slow tracks above that are elevated noise → UNK.
   (A first attempt "speed<30 alone" labeled elevated noise AND
   unseeded fast movers as SURF — caught via spy, reverted into this.)
3. **Publish/classify gate mismatch:** published at hits≥2 but classified
   at hits≥3 — every hits=2 row was UNK by construction. Gate now
   hits≥3 for both (TrackManager + TrackerCore aligned).

Plus **track merging** (the multi-ship root cause): the 35 dBsm ship
straddles adjacent 2.25° az cells (~1.9 km at 50 km) and spawned 2–3
persistent q=100 tracks. TrackerCore::update now merges pairs within
kMergeM=4 km (velocity must match only when BOTH are seeded; z not
compared, bar-quantized); higher-hits survives, loser disposed.
Verified offline: no replay sweep has two mature tracks near the same
truth; deterministic replay unchanged (1761 dets, 198 births, 211
deaths). Live-verified: pane = SURF ship + AIR fighters + a BAL +
transient UNK; no stuck classifications.

### 2026-07-20 (late, K5) — scenario-lifecycle decay explained; target RESPAWN added

- **User report:** track pane degraded over several minutes to "only an
  occasional UNK". Root cause is NOT the tracker: targets flew straight
  inbound FOREVER (no loop/respawn in TargetFleet), so the scenario
  drains as movers transit coverage and exit outbound (r⁴ SNR falloff).
  Offline replay shows the identical decay (~2 mature tracks at 600 s).
  The ASan-slowed window made it look worse (2–3x timing distortion).
  Normal build held published=3–5 for a full 5.3 min run.
- **FIX (approved): respawn recycling.** TargetFleet recycles any target
  whose ship-relative range exceeds `--respawn-range KM` (default 120,
  0 disables): fresh inbound trajectory from the initial distributions,
  anchored at the ship's CURRENT position (never drifts off), altitude
  reset from the profile (missiles dive), weave/orbit phase re-randomized.
  Same deterministic rng_ stream, so runs stay reproducible. Verified
  with `--respawn-range 30`: 69 recycles in 90 s, tracker healthy.
  New per-target `profile` index remembers which kProfiles entry resets z.
- **ID column fix VERIFIED on screen** (user-confirmed): full 4-digit
  pool IDs at 2x Retina.
- The 120 km-default demo pair now running doubles as the long windowed
  soak (final crash-closure criterion): watch for crashes past the old
  12–155 s window and for respawn lines from ~10 min on.

### 2026-07-20 (late, K5) — ASan confirmation + track-pane verdict

- **ASan confirmation (heat_ fix):** windowed + target_gen, ZERO reports
  in ~9 min, then stopped early (user priority switch). Pre-fix the
  same build died at startup on the first blips. A full 30-min ASan
  pass + a long non-sanitized windowed soak remain as the final
  closure runs.
- **"Empty Target Tracks pane" report — NOT a bug:** the user had been
  watching the ASan instance (2–3x slowdown distorts ALL tracker
  timing — revisit/coast/gates run on wall clock). Normal-build
  window: published ramps to 3–5 within ~10 s and holds, HMI follows
  1:1, user-confirmed healthy. With 8 targets, 3–5 concurrent tracks
  is the designed steady state (missiles invisible, drone late,
  hits≥2 gate suppresses the noise flicker that made old builds look
  "many").
- **Classification quirks (known, cosmetic):** SURF requires z<50 m but
  z is bar-quantized (ship reads ~R·sin3° ≈ 2.6 km at 50 km) → ship
  can never classify SURF, stays UNK. Tracks publish at hits≥2 but
  classify at hits≥3 → brief UNK window. Fast movers DO classify AIR
  once velocity seeds (~2 sweeps).
- **Track-table ID column fix:** clipped the 4th pool digit at 2x
  Retina scale (equal-stretch share vs FontGlobalScale-doubled text).
  Now `WidthFixed` from `ImGui::CalcTextSize("T0000")` + CellPadding —
  scale-aware (Panels.cpp).

### 2026-07-20 (late, K5) — ROOT CAUSE FOUND: heat_ brace-init shotgun

**The windowed-crash corruptor is identified, fixed, and every victim is
explained.** One line in `src/radar_app/ui/BScopeView.hpp`:

```cpp
std::vector<float> heat_{size_t(kAzBins) * kRangeBins, 0.0f};
```

Brace-init with `{count, value}`: `count` (92160) converts to `float`
**without narrowing**, so overload resolution prefers
`initializer_list<float>` — heat_ was a **TWO-element vector**
`{92160.0f, 0.0f}` (8-byte allocation, malloc-rounded to 16). Verified
with a 5-line test: brace → size 2, parens → size 92160.

`BScopeView::splat()` indexes `heat_[r*360 + a]` with r∈[0,255],
a∈[0,359] — the index math genuinely is bounded (why every audit passed:
they checked the indices, not the allocation). Against a 16-byte buffer,
every blip's 3×3 splat wrote/read floats at offsets up to ~368 KB past
it — a **shotgun into whatever malloc had placed there**: AppKit
titlebar arrays, CoreText font tables, Connext REDA pool blocks,
CF notification registrars, Metal pipeline state. Every victim (1–11)
was an innocent heap neighbor.

Caught by the 30-min ASan windowed run (`halt_on_error=1`): died at
STARTUP on the first blips — heap-buffer-overflow READ of size 4 at
`BScopeView.cpp:101`, address = 4 bytes past a freed 16-byte chunk
previously owned by `NSTitlebarView`'s `__NSArrayM` (the "smear anchor"
from the earlier ASan run — same mechanism, recycled chunk). Index
arithmetic + malloc rounding match exactly (heat_[5]: r=0, a=5 → 20
bytes = 4 past the 16-byte-rounded allocation).

**FIX (K5):** parens init, `heat_ = std::vector<float>(kAzBins*kRangeBins,
0.0f)`. Grep-verified the only brace-init-with-count in the codebase
(`noise_{0,sigma}` is a distribution — no initializer_list ctor, fine).

Why it fits ALL the evidence: windowed-only (splat runs only with the
UI; headless never constructs UiApp → E1/headless clean), stochastic
12–111 s timing (malloc layout × blip arrivals × when a victim touches
poisoned memory), TSan-clean (not a race — the cout race found and
fixed this session was unrelated noise), watchpoint-immune (the write
address moves with layout), ASan "caught only victims" before (runs
were too short / anchors pointed at recycled neighbors). Side effect
now obvious in hindsight: **the B-scope had never displayed a real
blip** (OOB writes never reached rgba_; texture was a flat gradient).

Confirmation status: 30-min windowed ASan confirmation run + a long
non-sanitized soak = the closure criteria. The experiment ladder
(no-titlebar, GL-throttle, swap-interval, bundle, Connext debug libs,
GLFW bump) is RETIRED — keep the runtime flags as diagnostics.

### 2026-07-20 (late, K5) — titlebar cluster + ASan anchor; thread-0 oddity closed

- **Victim count is now 10, and a cluster emerged:** 3 of 10 victims are in
  AppKit titlebar machinery (`NSTitlebarView`/`NSTitlebarContainerView`,
  SwiftUI titlebar layout). An ASan run produced a smear anchored to a
  **CoreText font table freed during titlebar section updates** (verbatim
  report not in repo — attach it to the next handoff if found in
  ~/Library/Logs/DiagnosticReports or the session log).
- **New experiment flag `--no-titlebar`** (uncommitted with this entry):
  `GLFW_DECORATED=FALSE` → undecorated window, removing the whole AppKit
  titlebar code path to test causality of that cluster. Wiring:
  `main.cpp` → `UiApp::set_undecorated` → window hint in `UiApp::init`.
- **Thread-0 oddity CLOSED (code-verified):** UI loop runs on the main
  thread (`main.cpp:165 app.run()`); the only WaitSet lives in
  CommandHandler's spawned thread; listeners run on Connext receive
  threads; no Connext entity is ever parked on thread 0 after startup.
  Victim #9's `KeventScanner`/kevent frames on `com.apple.main-thread`
  are therefore not explainable by our code — consistent with
  garbage-stack symbolication on a corrupted process.
- **Session-start hygiene:** killed a leftover `./build/target_gen`
  (PID 14397, ~20 min old, orphaned fragment of the prior ASan session)
  before any experiment, per the standing rule.
- **TSan run RESULT:** windowed + target_gen, app died at **155 s**
  (SIGBUS; extends the 12–111 s window — TSan slowdown applies). Exactly
  ONE race report rooted in src/: **unsynchronized `std::cout`** from
  `TrackManager::update_loop` vs `HmiUi::housekeeping_loop` (the
  heartbeat prints). Iostream-internal: can garble output, cannot
  corrupt AppKit/Connext heap → not the corruptor. **FIXED (K5):**
  `RADAR_LOG` (`src/common/Log.hpp`) — local ostringstream + one
  mutex-guarded emission per line, flushed; 13 sites across main /
  TrackManager / HmiUi / CommandHandler. target_gen prints left as-is
  (startup/one-shot only).
- **Victim #11 (under full TSan instrumentation):** SIGBUS in
  `CFXNotificationRegistrarFind` ← `_CFXNotificationPost` ←
  `__NSFinalizeThreadData` ← pthread TSD teardown on a **GCD worker
  (`_pthread_wqthread`)**. Same signature as ever: Apple-internal state
  walked into garbage, windowed-only — this time with TSan watching all
  our code and finding nothing that explains it. Suspect #1 (latent
  race in our code) substantially **EXONERATED** (caveat: TSan sees only
  races, only the paths a ~2.5 min run exercises).
- Ladder advances to #2: **30-min windowed ASan** with
  `ASAN_OPTIONS=halt_on_error=1` + target_gen (heap corruption TSan
  can't see: SPSC indices, vector growth, bridged MTLTexture lifetime).

### Current state — all working

- Build green; both apps run. UI is native Metal on macOS (no OpenGL
  anywhere; GL path kept under `#else` for other platforms). PPI blips
  are SNR-colored (with_alpha alpha-mask bug fixed); cursor range/bearing
  readout added; Retina layout fixed.
- Tracking VERIFIED via `tests/tracker_replay` (offline harness, no
  Connext needed): ship/bomber/fighter/decoy each hold one q=100 track
  with truth-grade speeds over 200 s. TrackerCore is DDS-free;
  TrackManager is a thin adapter.
- Beam: 3-bar elevation raster (3/14/25°), 100 Hz dwell, 1.6 s sweep,
  4.8 s per-bar revisit; tracker coast 12 s.

### Expected behavior — NOT bugs (don't "fix" these)

- Air targets vanish overhead above ~30.5° elevation (cone of silence)
  and are re-acquired outbound with a NEW track id.
- Slow ship's table row barely changes: reported position snaps to
  2.25° az cells (~2 km at 50 km); seeded speed ≈ 0; alt is
  bar-quantized (ship shows ~R·sin 3°). Physical, honest.
- The two missiles (11–12 km alt, −10 dBsm) stay invisible: SNR range
  (~9 km) < cone-entry range. Want them? More/higher elevation bars require
  a separate elevation-coverage change.
- Track rows step every ~4.8 s (bar revisit); no dead-reckoning between
  bursts (3-line patch available: publish x + vx·age in
  TrackManager::update_loop).

### Crash investigation — status and ranked next steps

Windowed radar_app SIGSEGV/SIGBUS at 12–111 s. **9 victims** now:
Connext REDA pools/database/writer (4), AppKit window bookkeeping (2),
QuartzCore commit, AGX Metal pipeline, HIToolbox, SwiftUI/AttributeGraph
(victim #9: `AG::LayoutDescriptor::Compare` / `AGGraphSetOutputValue`
during NSWindow display-cycle flush on the SwiftUI AsyncRenderer
thread, 45.7 s). Headless is ALWAYS clean (11 min ×2) with identical
DDS components/rates. Note: the last two windowed crashes both landed
at ~45 s (45.4, 45.7) — possibly converging on a period, possibly
coincidence; log exact wall-times of future crashes.

Ruled out: dispose path; GL→Metal shim (native Metal port crashed at
45 s anyway); **unbundled-app legacy AppKit path** (MACOSX_BUNDLE test
ran 2026-07-20 — crashed identically at 45.7 s, victim #9; bundle
experiment FAILED, keep the bundle anyway, it removes the tab-indexing
warning); our shared-state code (full audit: SPSC queues are textbook
single-producer/single-consumer with all-POD payloads — CmdReq/BlipView/
BeamView; DataBus stores all mutexed; DataWriter writes confined to one
WriterThread; listeners only queue-and-return). ASan caught only
victims; watchpoints inconclusive. Signature remains: random Apple
window-furniture + Connext internals tripping over corruption they
didn't cause — i.e. someone else's memory is being stepped on.

Surviving suspects, ranked:
1. **Latent race in our code (TOP PRIORITY — run this first)**:
   `cmake -S . -B build-tsan -DCMAKE_C_FLAGS="-fsanitize=thread"
   -DCMAKE_CXX_FLAGS="-fsanitize=thread"
   -DCMAKE_OBJCXX_FLAGS="-fsanitize=thread" && cmake --build
   build-tsan -j8` then run windowed with target_gen until it fires
   (or for 30 min). Any TSan report naming our code = found. TSan
   also watches Connext internals — expect noise from REDA; races
   rooted in src/ are the signal.
2. **Heap corruption TSan can't see** (SPSC indices, vector growth,
   bridged MTLTexture lifetime in MetalContext.mm): re-run the ASan
   build windowed with `ASAN_OPTIONS=halt_on_error=1` for a full 30 min
   with target_gen — past ASan runs were short and only caught victims.
3. **Below user space** (driver/WindowServer): restart wrapper for
   demos; headless + Admin Console/Studio for soaks; escalate to RTI
   with the 9 backtraces (7.7.0, arm64Darwin23clang16.0).

Oddity to glance at (don't theorize on truncated symbols): in victim
#9's crash log, thread 0 (com.apple.main-thread) shows Connext's
KeventScanner/kevent frames — verify the UI loop is really on the
main thread and no Connext waitset/scanner got parked there.

Behavior note (2026-07-20): the new TrackerCore only PUBLISHES tracks
at hits≥2 (noise suppression). A solo radar_app run (no target_gen)
now legitimately shows an EMPTY track table — old builds flickered
noise tracks, new build stays quiet.

**Empty-table incident (2026-07-20, UNRESOLVED):** bundled run, 45.7 s,
user saw ZERO tracks the whole run — with target_gen CONFIRMED running
non-stop. Not noise suppression: the ship (35 dBsm) must publish within
seconds. Static audit of the full chain read clean (DetectionEvent
listener registered; pending_ drained at 10 Hz; hits≥2 publish gate;
HmiUi TrackListener → tracks_ map → 5 Hz housekeeping → bus store →
Panels table). The same build family crashed inside this very thread
(victim #8, writer_.write), so early corruption wedging the pipeline is
a live hypothesis alongside a plain regression or a one-off.

HEARTBEAT BUILD deployed to localize it (first task for the next
session — run BEFORE TSan, 90 s windowed + target_gen, watch stdout):
- `[TrackManager] hb dets_in=N alive=N published=N handles=N` every 2 s
- `[HmiUi] hb tracks=N views=N` every 2 s
- `[TrackManager] reset consumed` if a spurious reset fires
Read it as a decision table:
  dets_in stuck at 0      → upstream: DetectionProcessor not publishing
                            or listener not firing
  dets_in>0, alive=0      → association failing in-app (heading? gate?)
  alive>0, published=0    → hits never reach 2 (fragmented detections)
  published>0, HMI tracks=0 → TargetTrack write/read path
  HMI tracks>0, table empty → Panels/rendering
  TM heartbeat STOPS mid-run, window keeps rendering → TrackManager
                            thread wedged = corruptor caught in the act;
                            empty table and crashes become ONE bug.

REJECTED with reasoning: waitsets/listener conversion (listeners run
identically in clean headless runs; UI thread never calls DDS); DDS
transport swap (DDS is the demo's purpose).

### Pending feature work (in order)

1. **target_gen stages**: Stage 0 PASSED (rtiddsspy: 16168 TargetTruth,
   404 ShipPosition, 0 dispose). Stage 1 first-contact checklist
   (expected detection ranges: ship/bomber immediate, decoy ~49 km,
   fighter ~28 km, missile ~9 km, drone ~5 km). Stage 2 scenario sweep.
   Stage 3 15–30 min soak.
2. Optional polish: dead-reckoning publish (above); track-table AZ
   column is RELATIVE bearing — label it or convert to true.

### Operational notes

- Build: `cd build && cmake .. && cmake --build . -j8` (re-configure
  when CMakeLists changes). Offline harness (no Connext):
  `cmake --build build --target tracker_replay && ./build/tracker_replay 300`.
- **STALE-BINARY TRAP (bit K5 twice in one session):** with
  MACOSX_BUNDLE on, `./build/radar_app` (plain) is a pre-bundle leftover
  that is NEVER relinked — only `radar_app.app/Contents/MacOS/radar_app`
  is current. A stale 3 AM binary sat next to the bundle all day; the
  K4 orphan (poisoned domain 0) was exactly such a leftover. Delete the
  plain binary on sight; README run instructions now point at the
  bundle.
- Run from repo root (QoS path is CWD-relative: qos/radar_qos.xml).
- Flags: `--headless --domain N --no-dispose --gl-throttle
  --swap-interval N` (last is a no-op with Metal).
- **Monitoring Library 2.0 is disabled in code** (2026-07-20, K5):
  `radds::disable_monitoring_lib()`, first line of both mains. Connext
  7.7 AUTO-ENABLES it on shared libs; its rti/dds/monitoring/* topics
  flooded Studio's System Visualization. The factory-QoS setting can NOT
  be done in our XML (the factory singleton reads the DEFAULT provider,
  not our QosProvider — learned the hard way: XML profile had zero
  effect). Override per-process with `RTI_MONITORING2_ENABLE=true`.
  Verify with `lsof -p <pid> | grep rtimonitoring` (empty = off) — a spy
  on domain 0 can't see the monitoring DP (domain 101 + RTI_o11y tag),
  and killed processes leave TRANSIENT_LOCAL ghosts until liveliness
  expires.
- .gitignore covers `build*/` — never commit build dirs or the
  fix-pack zip. macOS screenshot filenames contain U+202F — copy via
  glob. ASan needs `-fsanitize=address` in BOTH CMAKE_C_FLAGS and
  CMAKE_CXX_FLAGS (GLFW is C).

---

State as of 2026-07-20 (evening). Build is green; both apps run. One open
crash investigation. Read this first when resuming with a fresh context.

---

## 1. RESOLVED (2026-07-20, K5) — SIGSEGV after ~1 minute of runtime

Root cause: `heat_` brace-init in `BScopeView.hpp` → 2-element vector;
every `splat()` a heap shotgun. Full writeup in §0 (K5 entry). The
case log below is kept for reference.

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

### Third crash capture (2026-07-20 late) — inside the GPU driver itself

Windowed run (`--no-dispose`), 85 s, SIGBUS:

```
2  AGXMetalG16X    -[AGXG16XFamilyRenderContext setRenderPipelineState:]
4  AppleMetalOpenGLRenderer  GLDContextRec::setRenderState
7  GLEngine        glDrawArrays_IMM_Exec
13 AppKit          -[NSOpenGLContext flushBuffer]
14 radar_app       swapBuffersNSGL  (glfwSwapBuffers)
```

- The deprecated **OpenGL→Metal shim** (`AppleMetalOpenGLRenderer`) died
  inside the real GPU driver (`AGXMetalG16X`) executing our queued
  draws at swap time. Our GL calls are all legal (re-verified).
- All three windowed crashes are Apple-framework objects walked into
  garbage (titlebar views → window cache → Metal pipeline state); E1
  headless is clean TWICE. **PRIME SUSPECT: the GL-on-Metal path under
  our per-frame workload** — chiefly the B-scope's 368 KB texture
  re-upload every frame (~22 MB/s) + ImGui's per-frame buffer churn.
- New runtime flags to test GL-load causality WITHOUT rebuild toggles:
  - `--gl-throttle` — B-scope texture upload every 4th frame (15 Hz;
    invisible on the 4 s phosphor decay)
  - `--swap-interval N` — N=2 halves ALL GL traffic (30 fps)
- If either flag materially extends time-to-crash → GL shim load is
  causal; the durable fix is then porting rendering to
  `imgui_impl_metal` (bypasses the deprecated GL shim entirely).

### Experiment ladder (final form; in order)

1. **GL-load flags (one rebuild, then runtime A/B):**
   `./build/radar_app --gl-throttle` and, separately,
   `./build/radar_app --swap-interval 2`. Baseline crash window is
   ~12–90 s, so survival past ~5–10 min is a strong positive signal.
2. **ASan, windowed, solo** — covers our UI code + ImGui + ImPlot +
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
3. **No rebuild:** `NSZombieEnabled=YES ./build/radar_app` — a dangling
   NSView/NSWindow gets a console log naming the freed object's class
   and address. Then `MallocScribble=1 MallocPreScribble=1
   ./build/radar_app` — poisoned freed memory crashes the FIRST reader
   (0x55... pattern), closer to the cause.
4. **If the GL flags confirm causality:** the durable fix is porting
   rendering to `imgui_impl_metal` (bypasses the deprecated GL shim
   entirely); `--gl-throttle` alone is an acceptable permanent
   mitigation meanwhile.
5. **Connext debug libs** (`cmake -B build-dbg
   -DCMAKE_BUILD_TYPE=Debug`) — retained only as a formality; E1 makes
   Connext involvement unlikely.
6. **GLFW master bump** (`GIT_TAG 3.4` → `master` in CMakeLists) —
   cheap, but no relevant Cocoa fixes per the changelog.
7. TSan last (noisy with Connext internals, as before).

Report back: ASan report (or "ASan silent + zombie/scribble output").

---

## 2. Recently completed (don't redo)

- **Dedicated Beamformer DP** (2026-07-23): `Radar.Beamformer` now owns
  the actual array response. It subscribes scheduler intent on
  `Radar/BeamCommand` plus outage state on `Radar/CalibrationStatus`, then
  publishes `Radar/BeamPatternStatus` at 20 Hz. DetectionProcessor consumes
  that DDS topic for gain, width, pointing-error, and sidelobe effects instead
  of reading the in-process RMA mask or publishing beam status itself.
  CalibrationStatus retains its 1 Hz heartbeat and now also publishes within
  about 20 ms of degradation/RMA state changes. This produces the intended
  Studio topology: one BeamPatternStatus writer at Beamformer and readers at
  DetectionProcessor and HMI-UI.
- **RMA-offline, Tiers 2/3** (2026-07-23): `BeamPatternModel` calculates a
  deterministic 181-sample azimuth array-factor cut from the physical 4×4
  RMA mask, plus gain loss, 3 dB width, dominant sidelobes, and a bounded
  fallback-calibration boresight error. Beamformer publishes the metrics/cut
  at 20 Hz on reliable transient-local
  `Radar/BeamPatternStatus`; HMI-UI consumes the DDS topic for two B-scope
  views. The compact automatic outage view retains the moving response
  curtain and feature markers. The operator-selected **BEAM FORMATION**
  scenario control works while nominal and animates from a centered nominal
  pattern to a side-by-side nominal-versus-live comparison, then recenters
  nominal on recovery. Both plots share a slow 3D rotation around boresight.
  Its labeled display-spread accent makes a one-RMA change legible while the
  numeric metrics remain physical.
  During an outage, receiver synthesis applies the
  location-sensitive main-lobe response and permits only the two dominant,
  additionally attenuated sidelobes to create bounded strong-target ghosts.
  `beam_pattern_regression` covers nominal width, mask-position sensitivity,
  symmetric-error cancellation, monotonically cumulative gain loss, and
  all-offline safety.
- **RMA-offline, Tier 1** (2026-07-20, K5): 16 RMAs × 64 elements on a
  4×4 grid over the 32×32 face. `CMD_RMA_OFFLINE/ONLINE` (SystemCommand
  parameters "0".."15"/"all") drive `DataBus::rma_offline_mask`;
  CalibrationMonitor darkens the block (−60 dB), counts 64 failures per
  offline RMA and publishes the mask in CalibrationStatus
  (`rma_offline_mask`, @appendable); Beamformer turns that state into
  BeamPatternStatus and DetectionProcessor applies it to implant gain and
  azimuth response (el gate untouched — bar tiling). New ARRAY FACE pane:
  32×32 drift heatmap +
  RMA blocks, click a block to toggle it, ALL ONLINE button; health
  panel shows RMA OFF n/16. `target_gen --rma-offline N|all` scripts it
  (verified: command=6 params="3" → mask=8, DEGRADED, failed=64).
  tracker_replay unchanged (1761 dets deterministic).
- **Beam Schedule panel plots elevation** (2026-07-20 eve, K5): was
  az-only (0–360 sawtooth); the 3-bar el raster (3/14/25°, one bar per
  1.6 s revolution) was invisible. Now a second series on a right-side
  Y2 axis (0–30°), orange step line vs the green az sawtooth.

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
- **PPI black-dots fix** (2026-07-20 late): `with_alpha()` in
  PpiView.cpp masked colors with `IM_COL32_A_MASK` (= 0xFF000000, the
  ALPHA mask), discarding all RGB — every blip, track trail, velocity
  vector and the sweep fade rendered BLACK. Fixed with
  `~IM_COL32_A_MASK` + alpha clamp; blip core 1.8→2.2, halo /8→/6.
  Added a real cursor readout (CUR km / BRG°, bottom-left, when the
  mouse is inside the scope circle) — previously nothing tracked the
  mouse except wheel zoom.
- **Single-target fix** (2026-07-20 late): only the surface ship ever
  appeared in the track pane. Root cause: BeamScheduler parked the beam
  at el = 2.0° forever and DetectionProcessor's elevation gate was ±3°,
  so only targets between −1°…+5° elevation were ever implanted — the
  ship (0°) and nothing airborne (a fighter at 8 km alt is at ~16°
  elevation by the time it's in SNR range). Fix: two-bar elevation
  raster (3°/14°, toggles per revolution / sector bounce) + gate
  widened to ±6.5° → continuous cover deck-to-20.5°. Also fixed two
  latent tracker bugs exposed by fast movers: association now gates on
  the PREDICTED position (a 250 m/s fighter moves ~800 m per 3.2 s
  sweep > 750 m gate) and velocity is seeded from the first detection
  pair (track initiation). High-dive missiles above ~20° elevation at
  close range remain outside the cone — realistic surveillance ceiling.
- **Track flicker fix** (2026-07-20 late): tracks appeared one at a
  time, vanished, reappeared. The two-bar raster doubled per-cell
  revisit to 6.4 s > 5.0 s coast — every track was dropped before its
  next illumination and reborn after. Fast movers also never
  re-associated (un-seeded v=0 → ~800 m jump between sweeps > 750 m
  gate), so the previous seeding never fired. Fix: dwell 20→10 ms
  (sweep 1.6 s; per-bar revisit 3.2 s < coast), coast 5→9 s (survives
  a missed bar at the gate edge), un-seeded tracks get an age-growing
  capture gate (750 + 350·dt m), velocity seeds on the first
  cross-sweep association (`Track::v_init`). Side effect: UNK noise
  tracks now linger up to 9 s — cosmetic.
- **Frozen-track fix** (2026-07-20 late): tracks appeared but their
  range/az/speed/alt never changed before coasting out — no cross-sweep
  association ever succeeded. Three interacting causes: (1) single-pair
  velocity seeding used cell-quantized positions (az snaps to 2.25°
  dwell cells ≈ 1.2 km at 30 km) → seeded up to ~370 m/s of junk →
  next predicted association failed → fragmentation; (2) the 3°/14°
  elevation bars OVERLAPPED (7.5–9.5°) so mid-elevation targets
  alternated bars and reported z (= R·sin bar_el) jumped ~10 km;
  (3) z was in the association gate despite being bar-quantized.
  Fix: gate in XY only; bars now tile without overlap (el gate ±5.5°);
  velocity seeds from the SECOND cross-sweep hit over the birth-to-now
  span (clamped to 700 m/s); freshly-seeded tracks keep a widened gate
  for ~2 sweeps (cross_hits<4). Note: reported altitude remains
  bar-quantized by design (no monopulse) — ship alt reads ~R·sin 3°.
- **Offline tracker harness + frozen-track root causes** (2026-07-20
  late): extracted the DDS-free `TrackerCore` (association / α-β /
  coast) from TrackManager, which is now a thin DDS adapter.
  `tests/tracker_replay.cpp` replicates the beam raster, implant gates
  and CFAR peak-pick and drives the production core — builds with NO
  Connext (`cmake --build build --target tracker_replay`). First runs
  found what three blind fix rounds couldn't: (1) **β-term velocity
  explosion**: intra-burst dt clamps to 0.02 → β/dt = 10× any residual
  → multi-km/s junk speeds (the "BAL 335 m/s fighter"); β updates now
  gated to dt ≥ 0.25 s. (2) Slow-target rows legitimately LOOK frozen:
  reported positions snap to 2.25° az cells (~2 km at 50 km), so the
  ship's seeded speed ≈ 0 and range ticks only every ~30 s — physical
  quantization, not a stall. (3) Air targets vanished overhead at
  ~22 km (el cone top 19.5°): added a third bar at 25° → cone 30.5°,
  fighter held to ~14 km; per-bar revisit 4.8 s; coast 9→12 s for az
  dead stripes. Verified 200 s: ship/bomber/fighter/decoy all hold
  q=100 tracks with truth-grade speeds; table rows tick every sweep.
- **Metal renderer port** (2026-07-20 late): the windowed crashes (7+
  random victims, windowed-only, ASan-invisible, watchpoint-immune)
  pointed at Apple's deprecated OpenGL→Metal shim. radar_app on macOS
  now renders NATIVELY via Metal: GLFW_NO_API window + CAMetalLayer +
  imgui_impl_metal — no OpenGL context exists in the process, so the
  shim and its drivers are gone entirely. New MetalContext.hpp/.mm
  (device/queue/layer, per-frame drawable acquire + present, B-scope
  RGBA texture). The old GL path is retained under `#else` for
  non-Apple platforms. `--swap-interval` is a no-op on macOS
  (nextDrawable paces presents); `--gl-throttle` still works. NOTE:
  the ObjC++ was written without a macOS SDK to compile against —
  expect possible first-build fixes.
- **Metal port did NOT stop the crashes** (2026-07-20 late): 45 s
  SIGSEGV in TrackManager's TargetTrack writer (REDACursor / victim
  #8), same pattern as pre-port. GL-shim theory dead; the surviving
  windowed-only delta is Apple's compositing stack itself + the app
  being UNBUNDLED ("missing main bundle identifier", 3 early AppKit
  victims). Code audit meanwhile exonerated our shared state: SPSC
  queues single-producer (HmiUi listener / BeamScheduler), all bus
  stores mutexed, TrackManager writer single-threaded and identical
  in headless (11 min clean). Next experiments: (a) MACOSX_BUNDLE
  (done in CMakeLists — run
  ./build/radar_app.app/Contents/MacOS/radar_app); (b) TSan build
  (-fsanitize=thread in C/CXX/OBJCXX flags) to prove/disprove a race
  in our code; (c) if both fail, the corruptor is below user space →
  restart wrapper or headless+Studio.
- **HMI-UI is now a real DomainParticipant** (`Radar.HMI-UI`,
  `src/radar_app/components/HmiUi.{hpp,cpp}`): subscribes TargetTrack,
  DetectionEvent, ShipPosition (key 0), CalibrationStatus. Every panel is
  DDS-fed; **no dangling publishers**. radar_app had **8 participants** at
  this point and now has 9 with `Radar.Beamformer`. Producers no longer feed
  the UI in-process
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
  osx510; 9 participants exhaust 32 segments). Deferred: persistent
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
      map with 9+1 participants, TypeLookup decode, QoS match analysis,
      the three injection scenarios).
- [ ] Show dispose in action: track reset → instances vanish live in
      Studio (HMI-UI dispose path).
- [ ] Verify `--inject-qos-mismatch` and `--inject-type-mismatch` still
      behave with the re-keyed IDL (RogueReader/RogueWriter paths).
- [ ] Optional: persistent macOS sysv shmem config (KB osx510) if SHMEM
      is wanted back for the demo.
