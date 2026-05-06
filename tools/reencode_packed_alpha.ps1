# ─────────────────────────────────────────────────────────────────────────────
# reencode_packed_alpha.ps1
#
# Bulk-transcodes existing packed-alpha cache videos from HEVC 4:4:4 (rext)
# to H.264 4:2:0 full-range JPEG so they hit the NVDEC hardware fast-path
# on consumer NVIDIA GPUs (RTX 30/40 series and most laptops).
#
# Why: HEVC 4:4:4 falls off the NVDEC fast path → SW fallback at ~60-115ms
# per 4K frame → multi-second playback stalls at shot boundaries.
#
# What changes: Codec + pixel format only.  The packed-alpha *layout*
# (RGB top half, alpha-as-greyscale bottom half) is preserved exactly,
# and the existing decode/composite pipeline reads the new files without
# any code changes.
#
# Alpha precision: H.264 4:2:0 + full-range JPEG keeps all 256 alpha
# levels in the (full-resolution) luma channel.  Chroma subsampling
# does not affect alpha because alpha encodes as R=G=B=alpha greyscale,
# which puts U=V=neutral.
#
# Usage:
#     .\tools\reencode_packed_alpha.ps1                 # dry-run, lists files
#     .\tools\reencode_packed_alpha.ps1 -Execute        # actually transcode
#     .\tools\reencode_packed_alpha.ps1 -Execute -Qp 20 # higher quality
#     .\tools\reencode_packed_alpha.ps1 -Path assets\cache\animations\foo
#
# Output files replace originals atomically (write to .new, rename).
# Originals are kept as .bak unless -DeleteBackups is passed.
# ─────────────────────────────────────────────────────────────────────────────

param(
    [string]$Path = "assets\cache\animations",
    [string]$FfmpegPath = "third_party\ffmpeg\bin\ffmpeg.exe",
    [int]$Qp = 22,
    [switch]$Execute,
    [switch]$DeleteBackups,
    [switch]$Force,           # re-encode even files that look already-converted
    [int]$Parallel = 1        # >1 spawns concurrent jobs (NVENC sessions are limited!)
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $FfmpegPath)) {
    Write-Error "ffmpeg not found at $FfmpegPath"
    exit 1
}
if (-not (Test-Path $Path)) {
    Write-Error "Path not found: $Path"
    exit 1
}

$ffprobe = Join-Path (Split-Path $FfmpegPath) "ffprobe.exe"
if (-not (Test-Path $ffprobe)) {
    Write-Error "ffprobe not found alongside ffmpeg ($ffprobe)"
    exit 1
}

Write-Host ""
Write-Host "─── Packed-alpha re-encode ─────────────────────────────────────"
Write-Host "Scan path : $Path"
Write-Host "ffmpeg    : $FfmpegPath"
Write-Host "Target QP : $Qp (lower = higher quality, larger files)"
Write-Host "Mode      : $(if ($Execute) { 'EXECUTE' } else { 'DRY-RUN (use -Execute to apply)' })"
Write-Host "────────────────────────────────────────────────────────────────"

$files = Get-ChildItem -Path $Path -Recurse -Filter "*.mp4" -File
Write-Host "Found $($files.Count) .mp4 files."

$toConvert  = New-Object System.Collections.Generic.List[object]
$skipped    = 0
$errored    = 0

foreach ($f in $files) {
    # Probe codec + pix_fmt
    $info = & $ffprobe -v error -select_streams v:0 `
                -show_entries stream=codec_name,pix_fmt `
                -of default=noprint_wrappers=1:nokey=1 `
                $f.FullName 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $info) {
        Write-Warning "Could not probe: $($f.FullName)"
        $errored++
        continue
    }
    $lines = $info -split "`r?`n" | Where-Object { $_ -ne "" }
    $codec  = $lines[0]
    $pixfmt = $lines[1]

    $needs = $Force -or ($codec -eq "hevc") -or ($pixfmt -like "yuv444*")
    if (-not $needs) {
        $skipped++
        continue
    }
    $toConvert.Add([pscustomobject]@{
        Path   = $f.FullName
        Codec  = $codec
        PixFmt = $pixfmt
        SizeMB = [math]::Round($f.Length / 1MB, 1)
    })
}

Write-Host ""
Write-Host "Need re-encode : $($toConvert.Count)"
Write-Host "Already OK     : $skipped"
Write-Host "Probe errors   : $errored"
Write-Host ""

if ($toConvert.Count -eq 0) {
    Write-Host "Nothing to do."
    exit 0
}

if (-not $Execute) {
    Write-Host "Sample of files that would be converted:"
    $toConvert | Select-Object -First 10 | Format-Table -AutoSize
    Write-Host ""
    Write-Host "Re-run with -Execute to perform the conversion."
    exit 0
}

$totalMB = ($toConvert | Measure-Object SizeMB -Sum).Sum
Write-Host "Total source size: $([math]::Round($totalMB / 1024, 2)) GB"
Write-Host ""

$convertOne = {
    param($file, $ffmpeg, $qp)
    $src     = $file.Path
    $tmp     = "$src.new.mp4"
    $bak     = "$src.bak"

    # H.264 NVENC, all-intra, packed-alpha-friendly settings.
    # -color_range 2 / -colorspace 1 / -color_primaries 1 / -color_trc 1
    # = full-range JPEG, BT.709 — matches the new encoder.
    & $ffmpeg -y -hide_banner -loglevel error `
        -i $src `
        -an `
        -c:v h264_nvenc `
        -preset p4 -tune hq -profile:v high `
        -rc constqp -qp $qp `
        -bf 0 -g 2 -forced-idr 1 -force_key_frames "expr:1" `
        -pix_fmt yuv420p `
        -color_range 2 -colorspace 1 -color_primaries 1 -color_trc 1 `
        -movflags "+faststart+use_metadata_tags" `
        -metadata packed_alpha=1 `
        $tmp 2>&1
    $exit = $LASTEXITCODE

    if ($exit -ne 0 -or -not (Test-Path $tmp)) {
        if (Test-Path $tmp) { Remove-Item -Force $tmp -ErrorAction SilentlyContinue }
        return [pscustomobject]@{ Path = $src; Ok = $false; Error = "ffmpeg exit=$exit" }
    }
    # Atomic-ish swap
    Move-Item -Force $src $bak
    Move-Item -Force $tmp $src
    return [pscustomobject]@{ Path = $src; Ok = $true; BakPath = $bak }
}

$done = 0
$ok   = 0
$fail = 0
$results = New-Object System.Collections.Generic.List[object]
$start = Get-Date

if ($Parallel -le 1) {
    foreach ($item in $toConvert) {
        $done++
        Write-Host -NoNewline ("[{0,4}/{1}] {2} " -f $done, $toConvert.Count, $item.Path)
        $r = & $convertOne $item $FfmpegPath $Qp
        if ($r.Ok) {
            $ok++
            Write-Host "OK" -ForegroundColor Green
            $results.Add($r)
        } else {
            $fail++
            Write-Host "FAIL ($($r.Error))" -ForegroundColor Red
        }
    }
} else {
    Write-Host "Running with -Parallel $Parallel (NVENC has a session limit; reduce if you see errors)."
    $jobs = @()
    foreach ($item in $toConvert) {
        while ((Get-Job -State Running).Count -ge $Parallel) {
            Start-Sleep -Milliseconds 200
        }
        $j = Start-Job -ScriptBlock $convertOne -ArgumentList $item, $FfmpegPath, $Qp
        $jobs += $j
    }
    Write-Host "Waiting for $($jobs.Count) jobs..."
    $jobs | Wait-Job | Out-Null
    foreach ($j in $jobs) {
        $r = Receive-Job -Job $j
        if ($r.Ok) { $ok++; $results.Add($r) } else { $fail++ }
        Remove-Job $j
    }
}

$elapsed = (Get-Date) - $start

Write-Host ""
Write-Host "─── Re-encode complete ─────────────────────────────────────────"
Write-Host "OK     : $ok"
Write-Host "Failed : $fail"
Write-Host "Elapsed: $([math]::Round($elapsed.TotalMinutes, 1)) min"

if ($DeleteBackups -and $ok -gt 0) {
    Write-Host ""
    Write-Host "Deleting .bak files..."
    foreach ($r in $results) {
        if ($r.Ok -and (Test-Path $r.BakPath)) {
            Remove-Item -Force $r.BakPath
        }
    }
    Write-Host "Backups deleted."
} else {
    Write-Host ""
    Write-Host "Originals preserved as <name>.mp4.bak."
    Write-Host "After verifying playback, delete with:"
    Write-Host "  Get-ChildItem -Path '$Path' -Recurse -Filter '*.mp4.bak' | Remove-Item -Force"
}
