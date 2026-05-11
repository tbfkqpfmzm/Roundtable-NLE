$content = [System.IO.File]::ReadAllText("src/ui/panels/characters/ShotComposerUI.cpp")
$target = 'm_libraryTabs->addTab(bgTab, "Backgrounds");'
$idx = $content.IndexOf($target)
if ($idx -ge 0) {
    Write-Host "Found at index $idx"
    $after = $content.Substring($idx + $target.Length, 80)
    Write-Host "After: >>>$after<<<"
} else {
    Write-Host "Not found"
}
