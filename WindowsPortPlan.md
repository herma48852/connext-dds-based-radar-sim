# Windows 11 x64 Port Plan

## Objective

Port AesaRadarSim to native Windows 11 x64 using Visual Studio 2022 and RTI
Connext DDS 7.7.0 while preserving:

- The radar display and all operator controls.
- The 16-target, periodically respawning webinar scenario.
- DDS topology and Connext Studio observability.
- The existing headless regression suite.
- Repeatable demo startup and packaging.

This should be a moderate port rather than a rewrite. The project already:

- Separates the Apple Metal renderer from the non-Apple GLFW/OpenGL path.
- Includes an initial MSVC toolchain file.
- Excludes POSIX crash handling on non-POSIX platforms.
- Uses portable C++20 threading, atomics, and filesystem APIs.
- Provides three fast, headless CTest regressions.

The assumed target is native Windows 11 x64 with Visual Studio 2022 and
Connext DDS 7.7.0.

## 1. Establish the Windows development environment

Install:

- Visual Studio 2022 with:
  - Desktop development with C++
  - MSVC v143
  - Windows 11 SDK
  - CMake tools
- Git
- RTI Connext DDS 7.7.0 host package
- RTI Connext `x64Win64VS2017` target package
- A valid RTI license

RTI lists Windows 11 with the `x64Win64VS2017` architecture, including
Modern C++, UDPv4, multicast, and shared-memory support:

- [RTI Windows platform support](https://community.rti.com/static/documentation/connext-dds/current/doc/manuals/connext_dds_professional/platform_notes/platform_notes/Windows_Platforms.htm)
- [RTI Connext 7.7.0 installation guide](https://community.rti.com/static/documentation/connext-dds/7.7.0/doc/manuals/connext_dds_professional/installation_guide/installing.html)

Before configuring the project, use an RTI-enabled command prompt or run:

```bat
call "C:\Program Files\rti_connext_dds-7.7.0\resource\scripts\rtisetenv_x64Win64VS2017.bat"
```

The script configures `NDDSHOME`, the RTI Code Generator, and the Connext
runtime library path:

- [RTI environment setup](https://community.rti.com/static/documentation/connext-dds/current/doc/manuals/connext_dds_professional/getting_started_guide/cpp11/intro_pubsub_cpp.html)

### Exit criteria

- `rtiddsgen -version` succeeds.
- `%NDDSHOME%` identifies the 7.7.0 installation.
- `%NDDSHOME%\lib\x64Win64VS2017` exists.
- A basic RTI example builds and runs on the machine.

## 2. Normalize the CMake configuration

Add `CMakePresets.json` with named presets such as:

- `windows-vs2022-x64`
- `windows-relwithdebinfo`
- `windows-debug`

Recommended CMake changes:

1. Raise `cmake_minimum_required` from 3.20 to 3.21 because CMake introduced
   the Visual Studio 2022 generator in 3.21.
2. Use the native `Visual Studio 17 2022` generator with architecture `x64`.
3. Avoid specifying the generator platform both in the toolchain and through
   `-A x64`.
4. Retain FetchContent for GLFW, Dear ImGui, and ImPlot during the initial
   port. Do not introduce vcpkg until the baseline build is stable.
5. Apply `NOMINMAX` and `WIN32_LEAN_AND_MEAN` consistently to all project
   targets that may include Windows headers.
6. Build `RelWithDebInfo` first. Enable Debug only after confirming that the
   selected MSVC runtime and Connext libraries are compatible.
7. Keep the Apple Objective-C++ and Metal sources excluded from Windows.

CMake documents the native VS 2022 x64 form as:

```powershell
cmake -S . -B build\windows `
  -G "Visual Studio 17 2022" -A x64 `
  -DCONNEXTDDS_DIR="C:\Program Files\rti_connext_dds-7.7.0"
```

- [CMake Visual Studio 2022 generator](https://cmake.org/cmake/help/latest/generator/Visual%20Studio%2017%202022.html)

### Exit criteria

- CMake configures without platform, CRT, or Connext package warnings.
- `rtiddsgen.bat` generates the Modern C++ types during the build.
- FetchContent resolves all three graphics dependencies.
- The generated solution targets x64, not Win32 or ARM64.

## 3. Compile the headless targets first

Build targets in this order:

1. `target_scenario_regression`
2. `tracker_replay`
3. `ui_controls_smoke`
4. `target_gen`
5. `radar_app`

This isolates standard-library and compiler issues before combining DDS and
the Windows graphics driver.

Expected first-pass fixes include:

- MSVC warning and formatting differences.
- Windows `min` and `max` macro collisions.
- Multi-configuration output paths such as `RelWithDebInfo\`.
- QoS-file discovery relative to the executable or working directory.
- Connext DLL discovery at runtime.
- Any remaining assumptions about POSIX signals or process termination.

The existing POSIX backtrace implementation is already conditionally excluded
on Windows and should not block compilation. Windows crash dumps can be added
later without delaying the first functional build.

### Exit criteria

- All five targets compile and link in `RelWithDebInfo`.
- No Apple framework, Objective-C++, or Metal source enters a Windows target.
- `radar_app.exe --help` and `target_gen.exe --help` run successfully.

## 4. Make regression tests cross-platform deterministic

Run the multi-configuration CTest suite with:

```powershell
ctest --test-dir build\windows `
  -C RelWithDebInfo `
  --output-on-failure
```

The primary portability risk is `tracker_replay_regression`.
`std::normal_distribution` is not required to produce identical samples
across C++ standard-library implementations. MSVC may therefore generate
different golden event counts from macOS despite using the same engine seed.

Preferred resolution:

1. Replace the replay harness's library-dependent Gaussian generator with a
   small, explicitly defined deterministic algorithm.
2. Preserve one golden replay result across macOS and Windows.
3. Use invariant or tolerance checks only where unavoidable floating-point
   differences remain.
4. Avoid separate platform-specific golden counts unless no portable result
   can be achieved.

The target-scenario regression compares two locally seeded scenarios and
validates invariants rather than fixed coordinates, so it should be less
sensitive to standard-library differences. The ImGui smoke test should remain
display-free on Windows.

### Exit criteria

- All three CTest regressions pass on macOS and Windows.
- The tracker replay has one documented, reproducible golden result.
- No Windows test requires an interactive desktop or DDS participant.

## 5. Bring up the Windows OpenGL UI

Windows uses the existing GLFW/OpenGL 3.3 path. Validate on the actual webinar
hardware as early as possible:

- GLFW initialization and window creation.
- OpenGL 3.3 context creation.
- Dear ImGui and ImPlot rendering.
- B-scope texture creation, uploads, and cleanup.
- VSync and frame rate.
- Mouse hit testing for all scenario and RMA controls.
- Window resizing and minimize/restore behavior.
- Display scaling at 100%, 125%, 150%, and 200%.

Add a Windows application manifest declaring Per-Monitor V2 DPI awareness.
Consider starting maximized or sizing from the monitor work area because the
nominal 1800×1100 window will not fit comfortably on every 1080p display.

A vendor OpenGL driver is a deployment prerequisite. GLFW notes that Windows
context creation can fail if Microsoft's software GDI OpenGL implementation is
the only available implementation:

- [GLFW window and context guidance](https://www.glfw.org/docs/3.3/window_guide.html)

### Exit criteria

- The complete radar UI renders correctly on the target GPU.
- All automated control-smoke cases also work in a manual windowed check.
- Text and panels remain readable without clipping at supported DPI scales.
- A one-hour windowed run shows no resource growth, crash, or rendering loss.

## 6. Validate DDS locally on Windows

Run both applications on one Windows machine using an isolated domain:

```powershell
.\build\windows\RelWithDebInfo\radar_app.exe --domain 92
.\build\windows\RelWithDebInfo\target_gen.exe --domain 92 --targets 16
```

Validate:

- All expected participants discover.
- Sixteen targets publish, repeat, and respawn.
- Tracks populate and remain bounded.
- Every scenario button publishes `Radar/SystemCommand`.
- Sector scan changes `Radar/BeamCommand` azimuth and priority.
- Degrade and restore change `Radar/CalibrationStatus`.
- RMA selections and ALL ONLINE change `rma_offline_mask`.
- RESET disposes current `Radar/TargetTrack` instances and tracks reacquire.
- Connext Studio sees the documented topology and scenario transitions.

Keep UDPv4 as the initial transport. If discovery is blocked, create explicit
Windows Firewall rules for the applications rather than disabling the
firewall. Validate same-host communication first, then Studio on the same
machine, and finally Studio or the target generator on a second machine.

### Exit criteria

- Same-host DDS operation passes for 30 minutes.
- Connext Studio observes every scenario in `ConnextStudioDemo.md`.
- Multi-host discovery and data flow work on the intended webinar network.

## 7. Add Windows integration automation

Add a PowerShell smoke runner that:

1. Selects an isolated DDS domain.
2. Starts `radar_app --headless`.
3. Starts `target_gen --targets 16` on the same domain.
4. Captures each process's output separately.
5. Waits for discovery, detections, tracker heartbeats, and published tracks.
6. Stops both processes cleanly.
7. Fails on missing activity, fatal DDS diagnostics, crashes, or timeout.

Retain the existing ImGui control smoke test for deterministic button
behavior. Add a shorter manual GPU/DPI checklist because a headless runner
cannot validate the actual OpenGL driver, window manager, or monitor scaling.

### Exit criteria

- One PowerShell command performs the headless Windows integration smoke.
- Failures retain useful application logs.
- Repeated runs do not leave orphan processes or DDS participants.

## 8. Add Windows CI coverage

Public hosted CI generally will not have the licensed Connext SDK. Split CI
into two levels:

1. **Portable/core CI on `windows-2022`:** configure and run the DDS-free
   tracker, target-scenario, and UI-control tests. This may require CMake
   options that allow core tests to configure without Connext.
2. **Full Windows CI on a self-hosted runner:** install Connext and its license,
   build both applications, run all CTest cases, and execute the PowerShell DDS
   integration smoke.

Cache FetchContent dependencies where practical, but do not cache generated
IDL output across incompatible Connext installations.

### Exit criteria

- Every pull request runs the portable Windows tests.
- Release candidates run the full Connext build and DDS integration smoke.
- CI publishes logs and packaged binaries as artifacts.

## 9. Package a repeatable Windows demo

Create `run-demo.ps1` that:

- Sets the repository or installation working directory.
- Selects domain 92.
- Locates `qos\radar_qos.xml`.
- Verifies that Connext DLLs and the RTI license are available.
- Checks for stale demo processes.
- Launches `radar_app` and the 16-target generator.
- Writes separate timestamped logs.
- Stops both processes cleanly on request.

Produce a ZIP or CPack package containing:

- `radar_app.exe`
- `target_gen.exe`
- `qos\radar_qos.xml`
- Required Connext runtime DLLs, subject to RTI redistribution terms
- Any required Microsoft runtime components
- `run-demo.ps1`
- `ConnextStudioDemo.md`
- A short Windows README

Keep the applications as console executables initially so diagnostic output
remains visible during development and webinars. A GUI-subsystem launcher can
be added later if a console-free presentation is desired.

### Exit criteria

- The package runs on a clean Windows 11 x64 machine.
- It does not depend on the source tree or a Visual Studio installation.
- The demo launcher diagnoses missing DLL, license, QoS, and firewall setup.

## 10. Final acceptance gate

The Windows port is complete when all of the following are true:

- Visual Studio 2022 x64 builds without errors.
- All three CTest regressions pass on Windows and macOS.
- A 30-minute headless DDS integration soak passes.
- A 60-minute windowed run with 16 targets remains stable.
- Every scenario and RMA control works at multiple DPI scales.
- Connext Studio observes every documented command and downstream transition.
- Same-host and intended multi-host DDS configurations pass.
- The packaged demo runs on a clean Windows 11 x64 machine.
- Startup, operation, recovery, and troubleshooting are documented.

## Estimated effort

Assuming the Windows machine, Connext package, target libraries, license, GPU
driver, and network access are ready:

| Phase | Estimate |
|---|---:|
| Environment and CMake normalization | 0.5–1 day |
| First MSVC compile and link fixes | 1–2 days |
| Cross-platform regression stabilization | 1 day |
| OpenGL, DPI, and window validation | 1–2 days |
| DDS integration and Connext Studio validation | 1 day |
| Automation, CI, and packaging | 1–2 days |
| **Total** | **5–8 engineering days** |

Recommended branch name: `port/windows-x64`.
