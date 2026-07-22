function Resolve-RadarWindowsBuild {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,
        [string]$BuildDir,
        [ValidateSet("Debug", "RelWithDebInfo", "Release")]
        [string]$Configuration = "RelWithDebInfo",
        [bool]$BuildDirWasSpecified = $false
    )

    # A Windows process can inherit duplicate PATH/Path entries from a parent
    # environment block. MSBuild's .NET Framework tasks reject that block
    # before cl.exe starts, so collapse the aliases to one canonical entry.
    $pathKeys = @([Environment]::GetEnvironmentVariables().Keys |
        Where-Object { $_ -ieq "Path" })
    if ($pathKeys.Count -gt 1) {
        $pathValue = [Environment]::GetEnvironmentVariable("Path", "Process")
        foreach ($pathKey in $pathKeys) {
            [Environment]::SetEnvironmentVariable($pathKey, $null, "Process")
        }
        [Environment]::SetEnvironmentVariable("Path", $pathValue, "Process")
    }

    if (-not $BuildDir) {
        $BuildDir = Join-Path $RepoRoot "build\windows-x64"
    }
    $BuildDir = [System.IO.Path]::GetFullPath($BuildDir)

    function Find-RadarWindowsBinaries {
        $candidateDirs = @((Join-Path $BuildDir $Configuration), $BuildDir)
        if (-not $BuildDirWasSpecified) {
            $candidateDirs += (Join-Path $RepoRoot "bin")
        }

        foreach ($candidateDir in $candidateDirs) {
            $candidateRadar = Join-Path $candidateDir "radar_app.exe"
            $candidateTarget = Join-Path $candidateDir "target_gen.exe"
            if ((Test-Path -LiteralPath $candidateRadar) -and
                (Test-Path -LiteralPath $candidateTarget)) {
                return [pscustomobject]@{
                    BuildDir = if ($candidateDir -eq (Join-Path $RepoRoot "bin")) {
                        $RepoRoot
                    } else {
                        $BuildDir
                    }
                    ConfigDir = $candidateDir
                    RadarExe = $candidateRadar
                    TargetExe = $candidateTarget
                }
            }
        }
        return $null
    }

    $binaries = Find-RadarWindowsBinaries
    if ($binaries) {
        return $binaries
    }

    if ($BuildDirWasSpecified) {
        throw "Windows executables not found for configuration '$Configuration' in '$BuildDir'. Build that directory first."
    }
    if (-not (Test-Path -LiteralPath (Join-Path $RepoRoot "CMakePresets.json"))) {
        throw "Windows executables not found, and '$RepoRoot' is not a CMake source checkout."
    }
    if (-not (Get-Command cmake.exe -ErrorAction SilentlyContinue)) {
        throw "Windows executables not found and cmake.exe is not on PATH. Install CMake, then reopen Command Prompt."
    }

    Push-Location $RepoRoot
    try {
        if (-not (Test-Path -LiteralPath (Join-Path $BuildDir "CMakeCache.txt"))) {
            Write-Host "Windows build is not configured; running: cmake --preset windows-vs2022-x64"
            & cmake.exe --preset windows-vs2022-x64 | Out-Host
            if ($LASTEXITCODE -ne 0) {
                throw "CMake configure failed with exit code $LASTEXITCODE."
            }
        }

        Write-Host "Windows executables are missing; building configuration $Configuration."
        & cmake.exe --build $BuildDir --config $Configuration | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "CMake build failed with exit code $LASTEXITCODE."
        }
    }
    finally {
        Pop-Location
    }

    $binaries = Find-RadarWindowsBinaries
    if (-not $binaries) {
        throw "CMake completed, but radar_app.exe and target_gen.exe were not found in '$BuildDir'."
    }
    return $binaries
}
