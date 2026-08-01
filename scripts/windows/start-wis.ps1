<#
.SYNOPSIS
Starts RTI Web Integration Service for the Section 4 multi-topic live views.

.DESCRIPTION
Resolves repository-relative configuration and document-root paths, enables
WebSockets, and starts the aggregate RadarLiveViews configuration on port
18080 by default.

.PARAMETER ConnextDir
RTI Connext DDS installation. Defaults to CONNEXTDDS_DIR, NDDSHOME, or the
standard Windows 7.7.0 installation path.

.PARAMETER ConfigFile
WIS XML configuration. Defaults to config\radar_live_view_wis.xml.

.PARAMETER CfgName
Named web_integration_service configuration. Defaults to RadarLiveViews.

.PARAMETER DocumentRoot
Static web root. Defaults to the repository docs directory.

.PARAMETER ListeningPorts
WIS listening-port list. Defaults to 18080.

.PARAMETER Verbosity
WIS logging verbosity from 0 through 6. Defaults to 3.

.PARAMETER EnableBuiltinTopics
Enables WIS built-in-topic support.

.EXAMPLE
.\scripts\windows\start-wis.ps1

.EXAMPLE
.\scripts\windows\start-wis.ps1 -ListeningPorts 18081 -Verbosity 4
#>
[CmdletBinding()]
param(
    [string]$ConnextDir,
    [string]$ConfigFile,
    [string]$CfgName = "RadarLiveViews",
    [string]$DocumentRoot,
    [string]$ListeningPorts = "18080",
    [ValidateRange(0, 6)]
    [int]$Verbosity = 3,
    [switch]$EnableBuiltinTopics
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path

if (-not $ConnextDir) {
    if ($env:CONNEXTDDS_DIR) {
        $ConnextDir = $env:CONNEXTDDS_DIR
    } elseif ($env:NDDSHOME) {
        $ConnextDir = $env:NDDSHOME
    } else {
        $ConnextDir = Join-Path $env:ProgramFiles "rti_connext_dds-7.7.0"
    }
}
if (-not $ConfigFile) {
    $ConfigFile = Join-Path $repoRoot "config\radar_live_view_wis.xml"
}
if (-not $DocumentRoot) {
    $DocumentRoot = Join-Path $repoRoot "docs"
}

$wisLauncher = Join-Path $ConnextDir "bin\rtiwebintegrationservice.bat"
if (-not (Test-Path -LiteralPath $wisLauncher)) {
    throw "RTI Web Integration Service was not found at '$wisLauncher'. Set CONNEXTDDS_DIR or NDDSHOME, or pass -ConnextDir."
}
if (-not (Test-Path -LiteralPath $ConfigFile -PathType Leaf)) {
    throw "WIS configuration was not found at '$ConfigFile'."
}
if (-not (Test-Path -LiteralPath $DocumentRoot -PathType Container)) {
    throw "Document root was not found at '$DocumentRoot'."
}

$resolvedConfig = (Resolve-Path -LiteralPath $ConfigFile).Path
$resolvedDocumentRoot = (Resolve-Path -LiteralPath $DocumentRoot).Path
$wisArguments = @(
    "-cfgFile", $resolvedConfig,
    "-cfgName", $CfgName,
    "-enableWebSockets",
    "-documentRoot", $resolvedDocumentRoot,
    "-listeningPorts", $ListeningPorts,
    "-verbosity", $Verbosity
)
if ($EnableBuiltinTopics) {
    $wisArguments += "-enableBuiltinTopics"
}

Write-Host "Starting RTI Web Integration Service"
Write-Host "  config:        $resolvedConfig"
Write-Host "  configuration: $CfgName"
Write-Host "  document root: $resolvedDocumentRoot"
Write-Host "  listening:     $ListeningPorts"
Write-Host ""

& $wisLauncher @wisArguments
exit $LASTEXITCODE
