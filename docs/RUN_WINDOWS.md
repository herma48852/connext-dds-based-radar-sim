# Windows 11 Clean-Machine Runbook

This is the complete setup for a clean 64-bit Windows 11 development machine.
Follow the numbered steps in order. Do not skip a verification step.

Rules for this runbook:

- Enter every command in a regular **Command Prompt** (`cmd.exe`).
- Copy only the text inside each command block; do not type the explanatory
  text or the `C:\...>` prompt itself.
- Enter commands one line at a time. If a command reports an error, stop and
  fix that error before continuing.
- In Step 3, choose any existing folder where you want the repository cloned.
- The commands assume that RTI Connext DDS is installed in
  `C:\Program Files\rti_connext_dds-7.7.0`. If it is elsewhere, change only
  the `CONNEXTDDS_DIR` line in Step 4.
- Step 3 changes to the repository root. Stay in that directory for every
  remaining setup, build, test, launch, and packaging command.

## 1. Install Every Prerequisite

Install all of the following before opening Command Prompt:

1. **Visual Studio 2022**. In Visual Studio Installer, select the
   **Desktop development with C++** workload. Confirm that the installation
   includes **MSVC v143**, a **Windows 11 SDK**, and **C++ CMake tools for
   Windows**.
2. **Git for Windows**. The installer must make `git` available from Command
   Prompt.
3. **CMake 3.21 or newer**. Select the installer option that adds CMake to
   `PATH` so `cmake` is available from Command Prompt.
4. **RTI Connext DDS Professional 7.7.0**, including both the host tools and
   the **`x64Win64VS2017` target package**. Installing only the host tools is
   not enough. Install a valid RTI license using RTI's installer or launcher.
5. The current graphics driver from the computer's GPU vendor. The windowed
   radar requires OpenGL 3.3; the Microsoft fallback driver is insufficient.
6. Internet access to GitHub. Cloning and the first CMake configure download
   source dependencies.

Finish every installer and restart Windows if an installer requests it.

## 2. Open Command Prompt and Verify the Tools

Press the Windows key, type **Command Prompt**, and open it. Enter:

```bat
where git
git --version
where cmake
cmake --version
cmake --help | findstr /C:"Visual Studio 17 2022"
```

All five commands must succeed. The version output must show CMake 3.21 or
newer, and the last command must print a `Visual Studio 17 2022` generator.
If Windows says a command is not recognized or `where` cannot find it, stop,
repair that installation, and open a new Command Prompt.

## 3. Clone the Repository and Enter Its Root

First, use `cd /d` to enter any existing folder where you want the repository
directory created. For example, if you want the clone under `D:\Development`,
enter:

```bat
cd /d "D:\Development"
```

Replace `D:\Development` with your chosen folder. Confirm that the Command
Prompt now shows that location, then enter:

```bat
git clone https://github.com/herma48852/connext-dds-based-radar-sim.git
cd /d "connext-dds-based-radar-sim"
dir /b CMakePresets.json
```

Git creates `connext-dds-based-radar-sim` inside the folder you selected. The
last command must print `CMakePresets.json`, and the prompt should end in
`connext-dds-based-radar-sim>`. This is the repository root. Remember its full
path, and do not change directories again while following this runbook.

## 4. Configure and Verify Connext

Enter these commands in the same Command Prompt. If Connext was installed in
a different directory, edit the first line only.

```bat
set "CONNEXTDDS_DIR=C:\Program Files\rti_connext_dds-7.7.0"
set "NDDSHOME=%CONNEXTDDS_DIR%"
set "PATH=%CONNEXTDDS_DIR%\bin;%CONNEXTDDS_DIR%\lib\x64Win64VS2017;%PATH%"
if exist "%CONNEXTDDS_DIR%\bin" (echo OK: Connext host tools found) else (echo ERROR: Connext host tools not found)
if exist "%CONNEXTDDS_DIR%\lib\x64Win64VS2017" (echo OK: Connext target found) else (echo ERROR: x64Win64VS2017 target not found)
where rtiddsgen
rtiddsgen -version
```

Both checks must print `OK`, `where` must find `rtiddsgen.bat` beneath the
configured Connext directory, and `rtiddsgen -version` must finish with
`Done`. RTI versions this generator separately from the Connext product, so
Connext DDS 7.7.0 normally reports `rtiddsgen version 4.7.0`. The configure
output in Step 5 performs the product-version check and must report a suitable
RTI Connext DDS version of 7.7.0. If any check fails, stop. Correct
`CONNEXTDDS_DIR` or install the missing Connext host or target package before
continuing.

These environment variables apply only to this Command Prompt. Keep it open
until the build, tests, smoke test, and demo are finished.

## 5. Configure, Build, and Run the Regression Tests

Enter each command and wait for it to finish before entering the next:

```bat
cmake --preset windows-vs2022-x64
cmake --build --preset windows-relwithdebinfo
ctest --preset windows-relwithdebinfo
```

The configure command only creates Visual Studio project files; it does not
compile `radar_app.exe` or `target_gen.exe`. You must run the build command
before the smoke test. The supported smoke-test and demo launchers also detect
missing executables and run the configure/build steps automatically, so an
accidentally omitted build cannot leave the clean-machine workflow stuck.

The first configure can take several minutes while it downloads pinned GLFW,
Dear ImGui, and ImPlot sources. Success requires all of the following:

- Configure ends with `Configuring done` and `Generating done`.
- Build completes without an error.
- CTest reports `100% tests passed`.

During Windows configure, the `CMAKE_HAVE_LIBC_PTHREAD` probe is expected to
say `Failed`, and the `pthread`/`pthreads` library probes are expected to say
`not found`. They are Unix capability checks, not configure errors; continue
when CMake subsequently reports `Found Threads: TRUE`.

Do not continue if any of these checks fails. Because this is a clean clone,
do not copy in or reuse a `build` directory from another computer, operating
system, compiler, or generator.

## 6. Run the DDS Integration Smoke Test

On their first launch, Windows Defender Firewall may ask whether
`radar_app.exe` or `target_gen.exe` can communicate on the network. Allow each
application only on the network profile on which the demo will run. Respond
before the timed test finishes; if the prompt delays DDS discovery and the
test fails, resolve the prompt and rerun the command below. Do not disable the
firewall.

Enter:

```bat
scripts\windows\smoke-test.cmd -Domain 92
```

The test runs for about 20 seconds. It must finish with:

```text
PASS: Windows DDS integration smoke completed on domain 92.
```

The line after `PASS` gives the log directory. Do not launch the demo until
the smoke test passes.

## 7. Run the Demo

Enter:

```bat
scripts\windows\run-demo.cmd -Domain 92 -Targets 16
```

The radar window should open and Command Prompt should report that the AESA
radar demo is running. Leave that Command Prompt open. To stop both processes
cleanly, press ENTER or Q in Command Prompt, or close the radar window.

The `.cmd` files are the supported launch commands. They handle the bundled
implementation automatically. Do not open or invoke a `.ps1` file directly.
The demo launcher also supports `-Headless`, `-RunSeconds N`, and
`-StopExisting` when those behaviors are intentionally needed.

## Optional: Portable Tests Without Connext

This is not part of the clean-machine demo path above. On a machine without
Connext, the five portable regressions can be built and run from the
repository root with:

```bat
cmake --preset windows-portable
cmake --build --preset windows-portable-relwithdebinfo
ctest --preset windows-portable-relwithdebinfo
```

## GPU and DPI Checklist

Perform this manual check at 100%, 125%, 150%, and 200% display scaling:

1. Confirm the PPI, A-scope, B-scope, six lower panels, and all text render.
2. Resize, minimize, restore, and move the window between monitors.
3. Exercise all six scenario buttons, every RMA block, and ALL ONLINE.
4. Confirm the B-scope texture continues updating without OpenGL errors.
5. Run windowed with 16 targets for one hour and watch process memory.

The executable embeds a Per-Monitor V2 DPI manifest and reapplies UI scaling
when GLFW reports a monitor-scale change.

## DDS and Firewall

The shipped QoS uses UDPv4. Validate same-host operation first. For multi-host
operation, allow `radar_app.exe`, `target_gen.exe`, and Connext Studio through
Windows Defender Firewall on the intended network profile. Do not disable the
firewall globally. Use a unique DDS domain when multiple demos share a network.

Connext Studio should observe the topology and transitions described in
`ConnextStudioDemo.md`.

## Packaging

```bat
cmake --install build\windows-x64 --config RelWithDebInfo
cpack --config build\windows-x64\CPackConfig.cmake -C RelWithDebInfo
```

The ZIP is written to `build\windows-x64\package` and contains the
applications, QoS, scripts, and documentation. Connext
runtime DLLs and an RTI license are deliberately not redistributed. Install
the authorized Connext runtime or place its `x64Win64VS2017` DLL directory on
`PATH` on the destination machine.

## Troubleshooting

- **`git` or `cmake` is not recognized:** reinstall the missing tool with its
  `PATH` option enabled. Close Command Prompt, open a new one, and repeat the
  verification in Step 2.
- **Clone says the destination already exists:** do not clone over it and do
  not delete it. If it is the intended clone, enter that repository's full
  path with `cd /d "C:\full\path\to\connext-dds-based-radar-sim"`, run
  `dir /b CMakePresets.json`, and continue only if that file is printed.
- **Command Prompt was closed:** open a new Command Prompt and return to the
  location selected in Step 3 with
  `cd /d "C:\full\path\to\connext-dds-based-radar-sim"`. Repeat every command
  in Step 4, and then resume at the step that was interrupted.
- **CMake cannot find Visual Studio 2022 or MSVC:** open Visual Studio
  Installer, choose **Modify**, and install the workload and all three
  components listed in Step 1. Reboot if requested, then start again at
  Step 2.
- **The Connext host or target check prints `ERROR`:** correct the first line
  in Step 4 if Connext is installed elsewhere. If the directory is correct,
  modify the Connext installation and add the missing host tools or
  `x64Win64VS2017` target package.
- **CMake or `rtiddsgen` reports a license error:** activate or install the
  Connext 7.7.0 license with RTI's installer or launcher, then repeat Step 4.
- **CMake fails while downloading GLFW, ImGui, or ImPlot:** confirm that the
  machine can reach GitHub and that its proxy permits Git and CMake downloads,
  then rerun the first command in Step 5.
- **A launcher says the Windows executables are missing:** update the clone to
  the latest revision. The current launchers configure and build missing
  executables automatically. To build explicitly, run
  `cmake --build --preset windows-relwithdebinfo` from the repository root.
- **An executable reports a DLL load failure:** the current Command Prompt
  does not have the Connext runtime directory on `PATH`. Repeat all of Step 4
  in that same Command Prompt and retry.
- **The application reports a QoS load failure:** confirm that the prompt is
  still at the repository root and that `qos\radar_qos.xml` exists there.
- **The radar window reports a GLFW or OpenGL initialization failure:**
  install the current graphics driver directly from the GPU vendor and
  reboot. The Microsoft fallback OpenGL driver is insufficient.
- **The smoke test or demo reports no DDS discovery:** confirm that both
  applications use the same domain, close stale `radar_app.exe` and
  `target_gen.exe` processes, disconnect unnecessary VPNs, and check the
  per-application firewall rules. Do not disable the firewall globally.
