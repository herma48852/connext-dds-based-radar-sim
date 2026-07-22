# Windows 11 Runbook

## Prerequisites

- Windows 11 x64.
- Visual Studio 2022 with Desktop development with C++, MSVC v143, and a
  Windows 11 SDK.
- CMake 3.21 or newer and Git.
- RTI Connext DDS 7.7.0 with the `x64Win64VS2017` target package and a valid
  license.
- A vendor OpenGL 3.3-capable display driver for the windowed radar UI.

Open **Developer PowerShell for VS 2022** and initialize Connext in that
shell. A PowerShell prompt starts with `PS` (for example,
`PS C:\path\to\AesaRadarSim>`):

```powershell
$env:CONNEXTDDS_DIR = "C:\Program Files\rti_connext_dds-7.7.0"
$env:NDDSHOME = $env:CONNEXTDDS_DIR
$env:PATH = "$env:CONNEXTDDS_DIR\bin;$env:CONNEXTDDS_DIR\lib\x64Win64VS2017;$env:PATH"
rtiddsgen -version
```

If the prompt does not start with `PS` (for example,
`C:\Program Files\Microsoft Visual Studio\2022\Community>`), the shell is
Command Prompt (`cmd.exe`) even if it was opened from a PowerShell or Windows
Terminal shortcut. Use CMD syntax instead:

```bat
set "CONNEXTDDS_DIR=C:\Program Files\rti_connext_dds-7.7.0"
set "NDDSHOME=%CONNEXTDDS_DIR%"
set "PATH=%CONNEXTDDS_DIR%\bin;%CONNEXTDDS_DIR%\lib\x64Win64VS2017;%PATH%"
rtiddsgen -version
```

These variables apply only to the current shell. Run the remaining commands
from the repository root in the same shell.

## Configure and Build

```powershell
cmake --preset windows-vs2022-x64
cmake --build --preset windows-relwithdebinfo
ctest --preset windows-relwithdebinfo
```

The portable tests can be built on a machine without Connext:

```powershell
cmake --preset windows-portable
cmake --build --preset windows-portable-relwithdebinfo
ctest --preset windows-portable-relwithdebinfo
```

Do not reuse a CMake build directory created by another operating system,
compiler, or generator.

## Integration Smoke

The smoke runner starts both executables on an isolated domain, uses 16
targets, verifies live detections, saves separate logs, and lets both processes
shut down normally:

```powershell
.\scripts\windows\smoke-test.ps1 -Domain 92
```

From Command Prompt (`cmd.exe`):

```bat
scripts\windows\smoke-test.cmd -Domain 92
```

## Run the Demo

```powershell
.\scripts\windows\run-demo.ps1 -Domain 92 -Targets 16
```

From Command Prompt (`cmd.exe`), invoke the wrapper rather than the `.ps1`
file. CMD otherwise follows the machine's `.ps1` file association, which may
open the script in an editor:

```bat
scripts\windows\run-demo.cmd -Domain 92 -Targets 16
```

Press ENTER or Q in the launcher, or close the radar window, to stop both
processes cooperatively. Pass `-Headless` for a DDS-only run. The launcher
rejects stale demo processes unless `-StopExisting` is explicitly supplied.
`-RunSeconds N` provides a finite unattended demo run.

For manual terminals, add
`%NDDSHOME%\lib\x64Win64VS2017` to `PATH`, then run:

```powershell
.\build\windows-x64\RelWithDebInfo\radar_app.exe --domain 92
.\build\windows-x64\RelWithDebInfo\target_gen.exe --domain 92 --targets 16
```

`RADAR_QOS_FILE` can override QoS discovery. Otherwise each executable looks
beside itself and in the current working directory for `qos\radar_qos.xml`.

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

```powershell
cmake --install build\windows-x64 --config RelWithDebInfo
cpack --config build\windows-x64\CPackConfig.cmake -C RelWithDebInfo
```

The ZIP is written to `build\windows-x64\package` and contains the
applications, QoS, scripts, and documentation. Connext
runtime DLLs and an RTI license are deliberately not redistributed. Install
the authorized Connext runtime or place its `x64Win64VS2017` DLL directory on
`PATH` on the destination machine.

## Troubleshooting

- `rtiddsgen` missing: run the RTI environment script and verify
  `CONNEXTDDS_DIR`.
- DLL load failure: add the Connext target `lib` directory to `PATH`.
- QoS load failure: set `RADAR_QOS_FILE` to the absolute XML path.
- GLFW/OpenGL initialization failure: install the GPU vendor's current driver;
  the Microsoft GDI OpenGL fallback is insufficient.
- No discovery: check domain IDs, stale processes, network profile, VPNs, and
  per-application firewall rules.
