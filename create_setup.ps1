# ROUNDTABLE NLE v2 - Create Setup Script
# Builds a Release EXE and compiles the Inno Setup installer,
# then copies the setup.exe to the project root.
#
# Usage:
#   .\create_setup.bat
#   powershell -ExecutionPolicy Bypass -File create_setup.ps1
# ============================================================================

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $root

Write-Host "============================================" -ForegroundColor Cyan
Write-Host " Creating ROUNDTABLE Setup" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""

# ── Find cmake ─────────────────────────────────────────────────────────
$cmake = Get-ChildItem "$root\tools\cmake\cmake-*\bin\cmake.exe" -ErrorAction SilentlyContinue |
         Select-Object -First 1 -ExpandProperty FullName
if (-not $cmake) { $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source }
if (-not $cmake) {
    Write-Host "ERROR: CMake not found. Run setup.bat first." -ForegroundColor Red
    pause
    exit 1
}
Write-Host "Using cmake: $cmake" -ForegroundColor Gray
Write-Host ""

# ── Step 1: Configure Release ──────────────────────────────────────────
Write-Host "[1/4] Configuring Release build..." -ForegroundColor Yellow
& $cmake -B "$root\build" -DROUNDTABLE_DEV_BUILD=OFF
if ($LASTEXITCODE -ne 0) {
    Write-Host "CMake configure failed." -ForegroundColor Red
    pause
    exit 1
}
Write-Host ""

# ── Step 2: Build Release EXE ──────────────────────────────────────────
Write-Host "[2/4] Building Release EXE..." -ForegroundColor Yellow
& $cmake --build "$root\build" --config Release --target roundtable --parallel
if ($LASTEXITCODE -ne 0) {
    Write-Host "`nBuild failed." -ForegroundColor Red
    pause
    exit 1
}
Write-Host "`nBuild complete.`n" -ForegroundColor Green

# ── Step 3: Find ISCC (Inno Setup) ─────────────────────────────────────
Write-Host "[3/4] Finding Inno Setup compiler..." -ForegroundColor Yellow
$iscc = (Get-Command ISCC -ErrorAction SilentlyContinue).Source
if (-not $iscc) {
    $iscc = Get-ChildItem "C:\Program Files (x86)\Inno Setup*\ISCC.exe" -ErrorAction SilentlyContinue |
            Select-Object -First 1 -ExpandProperty FullName
}
if (-not $iscc) {
    Write-Host "ERROR: Inno Setup (ISCC.exe) not found." -ForegroundColor Red
    Write-Host "Install it from: https://jrsoftware.org/isdl.php" -ForegroundColor Yellow
    pause
    exit 1
}
Write-Host "Found: $iscc" -ForegroundColor Gray
Write-Host ""

# ── Determine version from git tag ─────────────────────────────────────
$version = "0.0.0"
try {
    $tag = git describe --tags --abbrev=0 2>$null
    if ($tag) { $version = $tag.TrimStart('v') }
} catch {}
Write-Host "Version: $version" -ForegroundColor Gray
Write-Host ""

# ── Step 4: Build installer ────────────────────────────────────────────
Write-Host "[4/4] Building installer..." -ForegroundColor Yellow
& $iscc "/DMyAppVersion=$version" "$root\installer.iss"
if ($LASTEXITCODE -ne 0) {
    Write-Host "`nInstaller build failed." -ForegroundColor Red
    pause
    exit 1
}
Write-Host ""

# ── Copy setup.exe to project root ─────────────────────────────────────
Write-Host "Copying setup.exe to project root..." -ForegroundColor Yellow
$setupFile = Get-ChildItem "$root\dist\Roundtable-*.exe" -ErrorAction SilentlyContinue |
             Sort-Object LastWriteTime -Descending |
             Select-Object -First 1
if ($setupFile) {
    Copy-Item -Path $setupFile.FullName -Destination "$root\$($setupFile.Name)" -Force
    Write-Host "Copied to: $root\$($setupFile.Name)" -ForegroundColor Green
} else {
    Write-Host "WARNING: Could not find setup exe in dist\ folder." -ForegroundColor Yellow
}
Write-Host ""

Write-Host "============================================" -ForegroundColor Cyan
Write-Host " Setup created successfully!" -ForegroundColor Green
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""

Pop-Location
pause
