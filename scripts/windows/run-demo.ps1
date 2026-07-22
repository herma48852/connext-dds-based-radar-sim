[CmdletBinding()]
param(
    [string]$BuildDir,
    [ValidateSet("Debug", "RelWithDebInfo", "Release")]
    [string]$Configuration = "RelWithDebInfo",
    [ValidateRange(0, 232)]
    [int]$Domain = 92,
    [ValidateRange(1, 256)]
    [int]$Targets = 16,
    [ValidateRange(0, 604800)]
    [int]$RunSeconds = 0,
    [string]$ConnextDir,
    [switch]$Headless,
    [switch]$StopExisting
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
    throw "Windows executables not found in '$configDir'."
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
$qosFile = @(
    (Join-Path $repoRoot "qos\radar_qos.xml"),
    (Join-Path $configDir "qos\radar_qos.xml")
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $qosFile) {
    throw "qos\radar_qos.xml was not found in the repository or beside the executables."
}

$stale = @(Get-Process -Name "radar_app", "target_gen" -ErrorAction SilentlyContinue)
if ($stale.Count -gt 0) {
    if (-not $StopExisting) {
        $ids = ($stale.Id -join ", ")
        throw "Stale radar demo processes are running (PIDs $ids). Stop them or pass -StopExisting."
    }
    $stale | Stop-Process -Force
}

$env:PATH = "$rtiLibDir;$env:PATH"
$env:RADAR_QOS_FILE = $qosFile
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$logDir = Join-Path $BuildDir "demo-logs\$stamp"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$stopFile = Join-Path $logDir "stop.signal"
$quotedStopFile = '"' + $stopFile + '"'

$radarArgs = @("--domain", $Domain, "--stop-file", $quotedStopFile)
if ($Headless) { $radarArgs += "--headless" }
$targetArgs = @("--domain", $Domain, "--targets", $Targets,
                "--stop-file", $quotedStopFile)
if ($RunSeconds -gt 0) {
    $radarArgs += @("--run-seconds", $RunSeconds)
    $targetArgs += @("--run-seconds", $RunSeconds)
}

$radar = $null
$target = $null
$processFailures = @()
try {
    $radar = Start-Process -FilePath $radarExe -PassThru -WindowStyle Hidden `
        -ArgumentList $radarArgs `
        -RedirectStandardOutput (Join-Path $logDir "radar.stdout.log") `
        -RedirectStandardError (Join-Path $logDir "radar.stderr.log")
    $null = $radar.Handle
    Start-Sleep -Seconds 2
    $target = Start-Process -FilePath $targetExe -PassThru -WindowStyle Hidden `
        -ArgumentList $targetArgs `
        -RedirectStandardOutput (Join-Path $logDir "target.stdout.log") `
        -RedirectStandardError (Join-Path $logDir "target.stderr.log")
    $null = $target.Handle

    Write-Host "AESA radar demo running on DDS domain $Domain (PIDs $($radar.Id), $($target.Id))."
    Write-Host "Press ENTER or Q to stop. Closing the radar window also stops the demo."
    Write-Host "Logs: $logDir"

    while (-not $radar.HasExited) {
        if ($RunSeconds -eq 0) {
            $keyAvailable = $false
            try { $keyAvailable = [Console]::KeyAvailable } catch { }
            if ($keyAvailable) {
                $key = [Console]::ReadKey($true)
                if ($key.Key -eq [ConsoleKey]::Enter -or $key.Key -eq [ConsoleKey]::Q) {
                    break
                }
            }
        }
        Start-Sleep -Milliseconds 200
        $radar.Refresh()
    }
}
finally {
    Set-Content -LiteralPath $stopFile -Value "stop" -NoNewline
    $deadline = (Get-Date).AddSeconds(15)
    foreach ($process in @($target, $radar)) {
        if (-not $process) { continue }
        while (-not $process.HasExited -and (Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 200
            $process.Refresh()
        }
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) {
            $processFailures += "$($process.ProcessName) exited with code $($process.ExitCode)"
        }
    }
    Write-Host "Demo stopped. Logs: $logDir"
}

if ($processFailures.Count -gt 0) {
    throw "$($processFailures -join '; '). See logs: $logDir"
}
