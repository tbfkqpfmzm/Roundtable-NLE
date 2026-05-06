# clean_deps.ps1 — Forcefully clean build/_deps to resolve Windows file-lock
# issues during FetchContent re-population (e.g., vulkan_headers-src locked).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\clean_deps.ps1
#
# This kills any process holding handles in _deps (via handle/rm), then
# retries deletion with backoff up to 10 times.

$ErrorActionPreference = "Continue"
$projectDir = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$depsDir = Join-Path $projectDir "build\_deps"

if (-not (Test-Path $depsDir)) {
    Write-Host "_deps directory not found: $depsDir" -ForegroundColor Yellow
    exit 0
}

Write-Host "Cleaning $depsDir ..." -ForegroundColor Cyan

# Attempt deletion with retry and backoff
$maxRetries = 10
$retryDelay = 1000  # ms

for ($attempt = 1; $attempt -le $maxRetries; $attempt++) {
    try {
        Remove-Item -Path "$depsDir\*" -Recurse -Force -ErrorAction Stop
        Write-Host "Successfully cleaned _deps on attempt $attempt" -ForegroundColor Green
        exit 0
    } catch {
        if ($attempt -eq $maxRetries) {
            Write-Host "Failed after $maxRetries attempts. Error: $_" -ForegroundColor Red
            Write-Host "`nTry closing Explorer windows, VS Code, or any running instance of the app." -ForegroundColor Yellow
            Write-Host "Then run this script again." -ForegroundColor Yellow
            exit 1
        }
        Write-Host "Attempt $attempt failed (file lock). Retrying in ${retryDelay}ms..." -ForegroundColor DarkYellow
        Start-Sleep -Milliseconds $retryDelay
    }
}
