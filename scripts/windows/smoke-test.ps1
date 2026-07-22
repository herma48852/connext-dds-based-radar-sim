[CmdletBinding()]
param(
    [string]$BuildDir,
    [ValidateSet("Debug", "RelWithDebInfo", "Release")]
    [string]$Configuration = "RelWithDebInfo",
    [ValidateRange(0, 232)]
    [int]$Domain = 92,
    [ValidateRange(12, 300)]
    [int]$DurationSeconds = 20,
    [string]$ConnextDir
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$buildDirWasSpecified = [bool]$BuildDir
if (-not $BuildDir) {
    $BuildDir = Join-Path $repoRoot "build\windows-x64"
}
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
$configDir = Join-Path $BuildDir $Configuration
$radarExe = Join-Path $configDir "radar_app.exe"
$targetExe = Join-Path $configDir "target_gen.exe"

if (-not (Test-Path -LiteralPath $radarExe) -and
    (Test-Path -LiteralPath (Join-Path $BuildDir "radar_app.exe"))) {
    $configDir = $BuildDir
}
if (-not $buildDirWasSpecified -and
    -not (Test-Path -LiteralPath (Join-Path $configDir "radar_app.exe")) -and
    (Test-Path -LiteralPath (Join-Path $repoRoot "bin\radar_app.exe"))) {
    $BuildDir = $repoRoot
    $configDir = Join-Path $repoRoot "bin"
}
$radarExe = Join-Path $configDir "radar_app.exe"
$targetExe = Join-Path $configDir "target_gen.exe"

if (-not (Test-Path -LiteralPath $radarExe) -or
    -not (Test-Path -LiteralPath $targetExe)) {
    throw "Windows executables not found in '$configDir'. Run: cmake --build --preset windows-relwithdebinfo"
}

if (-not $ConnextDir) {
    $ConnextDir = if ($env:CONNEXTDDS_DIR) { $env:CONNEXTDDS_DIR } else { $env:NDDSHOME }
}
if (-not $ConnextDir) {
    throw "Set CONNEXTDDS_DIR or NDDSHOME to the RTI Connext DDS 7.7.0 installation."
}
$rtiLibDir = Join-Path $ConnextDir "lib\x64Win64VS2017"
if (-not (Test-Path -LiteralPath $rtiLibDir)) {
    throw "Connext target libraries not found at '$rtiLibDir'."
}

$env:PATH = "$rtiLibDir;$env:PATH"
$qosFile = @(
    (Join-Path $repoRoot "qos\radar_qos.xml"),
    (Join-Path $configDir "qos\radar_qos.xml")
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $qosFile) {
    throw "qos\radar_qos.xml was not found in the repository or beside the executables."
}
$env:RADAR_QOS_FILE = $qosFile
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$logDir = Join-Path $BuildDir "smoke-logs\$stamp"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

$radarOut = Join-Path $logDir "radar.stdout.log"
$radarErr = Join-Path $logDir "radar.stderr.log"
$targetOut = Join-Path $logDir "target.stdout.log"
$targetErr = Join-Path $logDir "target.stderr.log"
$radar = $null
$target = $null

try {
    Write-Host "Starting radar_app and target_gen."
    Write-Host "On first launch, Windows Defender Firewall may ask for network access."
    Write-Host "Allow only the intended demo network profile; rerun this test if the prompt delays DDS discovery."
    $radar = Start-Process -FilePath $radarExe -PassThru `
        -ArgumentList @("--domain", $Domain, "--headless", "--run-seconds", $DurationSeconds) `
        -RedirectStandardOutput $radarOut -RedirectStandardError $radarErr
    $null = $radar.Handle
    Start-Sleep -Seconds 2
    $target = Start-Process -FilePath $targetExe -PassThru `
        -ArgumentList @("--domain", $Domain, "--targets", 16, "--run-seconds", ($DurationSeconds - 2)) `
        -RedirectStandardOutput $targetOut -RedirectStandardError $targetErr
    $null = $target.Handle

    $deadline = (Get-Date).AddSeconds($DurationSeconds + 30)
    while ((-not $radar.HasExited -or -not $target.HasExited) -and
           (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 250
        $radar.Refresh()
        $target.Refresh()
    }
    if (-not $radar.HasExited -or -not $target.HasExited) {
        throw "Integration smoke timed out."
    }
    $radar.WaitForExit()
    $target.WaitForExit()
    $radar.Refresh()
    $target.Refresh()
    if ($radar.ExitCode -ne 0 -or $target.ExitCode -ne 0) {
        throw "A process failed: radar=$($radar.ExitCode), target=$($target.ExitCode)."
    }

    $radarLog = (Get-Content -Raw -ErrorAction SilentlyContinue $radarOut) +
                (Get-Content -Raw -ErrorAction SilentlyContinue $radarErr)
    $targetLog = (Get-Content -Raw -ErrorAction SilentlyContinue $targetOut) +
                 (Get-Content -Raw -ErrorAction SilentlyContinue $targetErr)

    if ($radarLog -notmatch "\[radar_app\] alive") {
        throw "radar_app did not emit its liveness heartbeat."
    }
    if ($radarLog -notmatch "\[TrackManager\] hb dets_in=[1-9][0-9]*") {
        throw "No detections reached TrackManager."
    }
    if ($radarLog -match "(?i)fatal|deserialize sample error|worker exception") {
        throw "Fatal DDS or application diagnostics were found in the radar log."
    }
    if ($targetLog -notmatch "\[target_gen\] starting" -or
        $targetLog -notmatch "\[target_gen\] shutting down") {
        throw "target_gen did not complete its expected lifecycle."
    }

    Write-Host "PASS: Windows DDS integration smoke completed on domain $Domain."
    Write-Host "Logs: $logDir"
}
finally {
    foreach ($process in @($target, $radar)) {
        if ($process -and -not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
    }
}
