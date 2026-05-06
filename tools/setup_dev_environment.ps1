# DEPRECATED — Use setup.ps1 in the project root instead.
#
# This script used vcpkg, but the project now uses CMake FetchContent for all
# C++ dependencies. The only external requirements are:
#   - Visual Studio 2022 with C++ Desktop workload
#   - Git
#   - Python 3.x (for aqtinstall Qt installer)
#
# Run from project root:
#   powershell -ExecutionPolicy Bypass -File setup.ps1

Write-Host "This script is deprecated. Use setup.ps1 in the project root instead." -ForegroundColor Yellow
Write-Host ""
Write-Host "  cd $PSScriptRoot\.." -ForegroundColor Cyan
Write-Host "  powershell -ExecutionPolicy Bypass -File setup.ps1" -ForegroundColor Cyan
Write-Host ""
