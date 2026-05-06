# ROUNDTABLE NLE v2 - Build Script
# ============================================================================
# Run setup.ps1 first, then use this to build.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File build.ps1
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Config Debug
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Clean  # clean before build (fixes LNK2011 stale PCH)
# ============================================================================

param(
    [ValidateSet("Release", "Debug", "RelWithDebInfo")]
    [string]$Config = "Release",
    [switch]$Clean,
    [switch]$CleanDeps
)

$ErrorActionPreference = "Stop"
$projectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $projectDir

# Clean stale FetchContent _deps directories (fixes Windows file-lock errors)
# like "Error removing directory .../vulkan_headers-src".
if ($CleanDeps) {
    Write-Host "Cleaning FetchContent _deps directories..." -ForegroundColor Yellow
    $depsDir = Join-Path $projectDir "build\_deps"
    if (Test-Path $depsDir) {
        $maxRetries = 5
        for ($attempt = 1; $attempt -le $maxRetries; $attempt++) {
            try {
                Remove-Item -Path "$depsDir\*" -Recurse -Force -ErrorAction Stop
                Write-Host "  _deps cleaned successfully." -ForegroundColor Green
                break
            } catch {
                if ($attempt -eq $maxRetries) {
                    Write-Host "  Failed to clean _deps (file lock). Try closing Explorer / VS Code / the app." -ForegroundColor Red
                    Write-Host "  Then run: tools\clean_deps.ps1" -ForegroundColor Yellow
                } else {
                    Write-Host "  Retrying (attempt $attempt)..." -ForegroundColor DarkYellow
                    Start-Sleep -Milliseconds 1000
                }
            }
        }
    }
}

# Find CMake (local project cmake first, then VS2022, then PATH)
$cmakeExe = $null

# 1. Check local cmake in tools/cmake/
$localCmake = Get-ChildItem "$projectDir\tools\cmake\cmake-*\bin\cmake.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($localCmake) { $cmakeExe = $localCmake.FullName }

# 2. Try VS2022 bundled cmake
if (-not $cmakeExe) {
    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vsWhere) {
        $vsInstallDir = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($vsInstallDir) {
            $vsCmake = Get-ChildItem "$vsInstallDir\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -ErrorAction SilentlyContinue
            if ($vsCmake) { $cmakeExe = $vsCmake.FullName }
        }
    }
}

# 3. Try system PATH
if (-not $cmakeExe) {
    $sysCmake = Get-Command cmake -ErrorAction SilentlyContinue
    if ($sysCmake) { $cmakeExe = $sysCmake.Source }
}

if (-not $cmakeExe) {
    Write-Host "CMake not found. Run setup.ps1 first." -ForegroundColor Red
    exit 1
}

# Prepend bundled cmake directory to PATH so FetchContent sub-builds
# (e.g. spdlog) also find the correct cmake version instead of falling
# back to the system PATH version (which may be too old).
$cmakeBinDir = Split-Path -Parent $cmakeExe
$env:PATH = "${cmakeBinDir};${env:PATH}"

# Also fix the CMAKE_COMMAND in the existing CMakeCache.txt if it points
# to the wrong cmake (e.g. conda's cmake that was on PATH during setup).
# FetchContent sub-builds use this value and fail if it's too old.
$cmakeCachePath = "build\CMakeCache.txt"
if (Test-Path $cmakeCachePath) {
    $currentCmd = Select-String -Path $cmakeCachePath -Pattern "^CMAKE_COMMAND:" -Quiet
    if ($currentCmd) {
        $expectedCmd = $cmakeExe -replace '\\', '/'
        $content = Get-Content $cmakeCachePath
        $updated = $false
        for ($i = 0; $i -lt $content.Count; $i++) {
            if ($content[$i] -match '^CMAKE_COMMAND:') {
                $oldVal = $content[$i]
                $content[$i] = "CMAKE_COMMAND:INTERNAL=$expectedCmd"
                if ($oldVal -ne $content[$i]) {
                    Write-Host "  Fixed CMAKE_COMMAND in CMakeCache.txt: $expectedCmd" -ForegroundColor Yellow
                    $updated = $true
                }
                break
            }
        }
        if ($updated) {
            $content | Set-Content $cmakeCachePath
        }
    }
}

if (-not (Test-Path "build\CMakeCache.txt")) {
    Write-Host "Build not configured. Run setup.ps1 first." -ForegroundColor Red
    exit 1
}

# Reconfigure if Qt6_DIR is NOTFOUND (stale/broken cache)
$qt6Line = Select-String -Path "build\CMakeCache.txt" -Pattern "Qt6_DIR:PATH=Qt6_DIR-NOTFOUND" -Quiet
if ($qt6Line) {
    Write-Host "Qt6 not found in cache, reconfiguring..." -ForegroundColor Yellow
    & $cmakeExe -B build -DCMAKE_PREFIX_PATH="$projectDir\third_party\qt\6.8.3\msvc2022_64\lib\cmake"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Reconfiguration failed." -ForegroundColor Red
        exit 1
    }
}

Write-Host "Building ROUNDTABLE ($Config)..." -ForegroundColor Cyan
if ($Clean) {
    Write-Host "Clean build requested - removing stale objects..." -ForegroundColor Yellow
    & $cmakeExe --build build --config $Config --target clean
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Clean failed, continuing anyway..." -ForegroundColor Yellow
    }
}
& $cmakeExe --build build --config $Config --parallel

if ($LASTEXITCODE -eq 0) {
    # Verify the executable was actually produced
    $exeFound = $false
    $exePath = ""
    if (Test-Path "build\bin\$Config\roundtable.exe") {
        $exeFound = $true
        $exePath = "build\bin\$Config\roundtable.exe"
    } elseif (Test-Path "build\bin\Release\roundtable.exe") {
        $exeFound = $true
        $exePath = "build\bin\Release\roundtable.exe"
    } elseif (Test-Path "build\bin\Debug\roundtable.exe") {
        $exeFound = $true
        $exePath = "build\bin\Debug\roundtable.exe"
    }

    Write-Host ""
    if ($exeFound) {
        Write-Host "Build successful! Executable: $exePath" -ForegroundColor Green
        Write-Host "Run with: .\launch.bat" -ForegroundColor Cyan
    } else {
        Write-Host "WARNING: Build reported success but roundtable.exe was not found!" -ForegroundColor Red
        Write-Host "The CMake cache may have stale/broken dependencies." -ForegroundColor Yellow
        Write-Host "Try deleting the build\ folder and running setup.ps1 + build.ps1 again." -ForegroundColor Yellow
        exit 1
    }
} else {
    Write-Host ""
    Write-Host "Build failed. Check errors above." -ForegroundColor Red
    exit 1
}

Pop-Location
