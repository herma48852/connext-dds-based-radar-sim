[CmdletBinding()]
param(
    [string]$BuildDir,
    [ValidateSet("Debug", "RelWithDebInfo", "Release")]
    [string]$Configuration = "RelWithDebInfo",
    [ValidateRange(0, 232)]
    [int]$Domain = 92,
    [ValidateRange(1, 256)]
    [int]$Targets = 32,
    [ValidateRange(0, 604800)]
    [int]$RunSeconds = 0,
    [string]$ConnextDir,
    [switch]$Headless,
    [switch]$DisableSub3km,
    [switch]$TargetControl,
    [switch]$StopExisting
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$buildDirWasSpecified = [bool]$BuildDir

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
$env:CONNEXTDDS_DIR = $ConnextDir

. (Join-Path $PSScriptRoot "ensure-build.ps1")
$resolvedBuild = Resolve-RadarWindowsBuild -RepoRoot $repoRoot `
    -BuildDir $BuildDir -Configuration $Configuration `
    -BuildDirWasSpecified $buildDirWasSpecified
$BuildDir = $resolvedBuild.BuildDir
$configDir = $resolvedBuild.ConfigDir
$radarExe = $resolvedBuild.RadarExe
$targetExe = $resolvedBuild.TargetExe
$controlExe = $resolvedBuild.ControlExe
$controlDomain = ($Domain + 1) % 233

$qosFile = @(
    (Join-Path $repoRoot "qos\radar_qos.xml"),
    (Join-Path $configDir "qos\radar_qos.xml")
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $qosFile) {
    throw "qos\radar_qos.xml was not found in the repository or beside the executables."
}

$stale = @(Get-Process -Name "radar_app", "target_gen", "target_control" -ErrorAction SilentlyContinue)
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
if ($DisableSub3km) { $radarArgs += "--disable-sub-3km" }
$targetArgs = @("--domain", $Domain, "--control-domain", $controlDomain,
                "--targets", $Targets,
                "--stop-file", $quotedStopFile)
$controlArgs = @("--domain", $controlDomain, "--stop-file", $quotedStopFile)
if ($RunSeconds -gt 0) {
    $radarArgs += @("--run-seconds", $RunSeconds)
    $targetArgs += @("--run-seconds", $RunSeconds)
    $controlArgs += @("--run-seconds", $RunSeconds)
}

$radar = $null
$target = $null
$control = $null
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

    if ($TargetControl) {
        Start-Sleep -Seconds 1
        $control = Start-Process -FilePath $controlExe -PassThru -WindowStyle Hidden `
            -ArgumentList $controlArgs `
            -RedirectStandardOutput (Join-Path $logDir "control.stdout.log") `
            -RedirectStandardError (Join-Path $logDir "control.stderr.log")
        $null = $control.Handle
    }

    $processIds = @($radar.Id, $target.Id)
    if ($control) { $processIds += $control.Id }
    Write-Host "AESA radar demo running on simulation domain $Domain (PIDs $($processIds -join ', '))."
    if ($control) {
        Write-Host "Target-control UI running independently on control domain $controlDomain."
    } else {
        Write-Host "Optional target UI: `"$controlExe`" --domain $controlDomain"
    }
    Write-Host "Press ENTER or Q to stop all launched processes. Closing the radar window also stops them."
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
    foreach ($process in @($control, $target, $radar)) {
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
