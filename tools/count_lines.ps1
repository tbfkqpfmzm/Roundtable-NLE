param([string]$BaseDir = ".")

$results = [System.Collections.ArrayList]::new()
foreach ($dir in @('src', 'tests', 'shaders')) {
    $root = Join-Path $BaseDir $dir
    if (!(Test-Path $root)) { continue }
    Get-ChildItem -Path $root -Recurse -Include *.cpp,*.h,*.comp,*.frag,*.vert,*.glsl |
        Where-Object { $_.FullName -notmatch '\\third_party\\' -and $_.FullName -notmatch '\\_NEW\\' -and $_.FullName -notmatch '\\build\\' } |
        ForEach-Object {
            $lines = [System.IO.File]::ReadAllLines($_.FullName).Count
            $rel = $_.FullName.Replace($BaseDir + '\', '').Replace('\', '/')
            [void]$results.Add([PSCustomObject]@{Lines=$lines; File=$rel})
        }
}

$total = $results.Count
$over300 = $results | Where-Object { $_.Lines -gt 300 } | Sort-Object Lines -Descending
Write-Output "$total files scanned, $($over300.Count) over 300 lines"
foreach ($r in $over300) {
    Write-Output "$($r.Lines) $($r.File)"
}
