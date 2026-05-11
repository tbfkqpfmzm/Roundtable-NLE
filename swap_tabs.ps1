$f = "C:\Users\rmdou\Desktop\TIER_LIST_PROGRAM_NEW\src\ui\panels\library\LibraryPanel.cpp"
$c = Get-Content $f -Raw
$i1 = $c.IndexOf("// Tab 2: NikkeBKG")
$i2 = $c.IndexOf("// Tab 3: Backgrounds")
$i3 = $c.IndexOf("// Tab 3: Videos")
$before = $c.Substring(0, $i1)
$nikke = $c.Substring($i1, ($i2 - $i1))
$bg = $c.Substring($i2, ($i3 - $i2))
$after = $c.Substring($i3)
$nikke = $nikke.Replace("Tab 2: NikkeBKG", "Tab 3: NikkeBKG")
$bg = $bg.Replace("Tab 3: Backgrounds", "Tab 2: Backgrounds")
$result = $before + $bg + $nikke + $after
[System.IO.File]::WriteAllText($f, $result)
Write-Host "Tabs swapped successfully"
