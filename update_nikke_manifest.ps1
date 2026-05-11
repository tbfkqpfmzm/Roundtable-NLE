# update_nikke_manifest.ps1
# Regenerates assets/nikke_backgrounds.json by fetching the file list
# from the public MEGA folder and decrypting names via the web page.
#
# Usage:  .\update_nikke_manifest.ps1
#
# Requires:  PowerShell, a web browser (uses the MEGA web UI)

Write-Host "=== Nikke Backgrounds Manifest Updater ===" -ForegroundColor Cyan
Write-Host ""
Write-Host "To regenerate the manifest when new backgrounds are added:" -ForegroundColor Yellow
Write-Host "1. Open https://mega.nz/folder/V55C3T6C#02N6WUjiEQ8hQv6PhZuMkg in a browser" -ForegroundColor White
Write-Host "2. Open DevTools (F12) and go to the Console tab" -ForegroundColor White
Write-Host "3. Run the following JavaScript:" -ForegroundColor White
Write-Host ""
Write-Host 'copy (() => {' -ForegroundColor Green
Write-Host '  const files = window.M.v;' -ForegroundColor Green
Write-Host '  const pngFiles = files' -ForegroundColor Green
Write-Host "    .filter(f => f.name?.toLowerCase().endsWith('.png'))" -ForegroundColor Green
Write-Host "    .map(f => ({name: f.name, handle: f.h, size: f.s}))" -ForegroundColor Green
Write-Host "    .sort((a, b) => a.name.localeCompare(b.name));" -ForegroundColor Green
Write-Host '  return JSON.stringify(pngFiles, null, 2);' -ForegroundColor Green
Write-Host '})();' -ForegroundColor Green
Write-Host ""
Write-Host "4. Copy the output and save it to assets/nikke_backgrounds.json" -ForegroundColor White
Write-Host ""
