# The Podslicer accent: muted arctic blue, replacing upstream's Material teal.
#
# This is the AUTHORITY on the accent mapping, and it is idempotent: after any
# upstream rebase reintroduces teal literals, run it again and the fork's colour
# identity is restored in one pass. That is how a 2,300-literal identity change
# stays a one-command rebase cost instead of a hand-merge.
#
# Deliberately untouched: src\libslic3r (its teal literals are DEFAULT FILAMENT
# COLOURS - material data that reaches G-code - except ColorRGBA::ORCA() in
# Color.hpp, edited by hand), and BitmapCache.cpp's "#00AE42" REPLACE KEY, which
# names the source colour being rewritten at SVG load, not a colour to show.
param([switch]$DryRun)

$repo = Split-Path $PSScriptRoot -Parent

# hex-string forms (case-insensitive), also matched inside SVG/CSS/JS
$hex = [ordered]@{
    '#009688' = '#4F87A5'   # accent base
    '#009687' = '#4F87A5'   # drifted typo of the base, found in tree 15 Aug 2026
    '#26A69A' = '#6B9DB8'   # hover
    '#00675[bB]' = '#3D6B85' # dark-mode base
    '#008172' = '#4F87A5'   # dark-mode hover
    '#00FFD4' = '#8FD0EA'   # pressed flash
    '#52C7B8' = '#7FB8D4'   # light tint (BitmapCache #00FF00 target)
    '#00AE42' = '#4F87A5'   # Bambu-green stragglers
    '#BFE1DE' = '#C0D4E2'   # pale selection wash (board_sel, tab pills)
    '#E5F0EE' = '#E5EEF5'   # paler hover wash
    '#009789' = '#4F87A5'   # AboutDialog link, another drifted base
    '#22[bB][fF][bB]0' = '#5FA5C9' # btn_confirm dark-mode focus ring
}
# 0xRRGGBB int forms (StateColor::append(unsigned long) swaps to wx layout itself)
$int = [ordered]@{
    '0x009688' = '0x4F87A5'; '0x009687' = '0x4F87A5'; '0x26A69A' = '0x6B9DB8'
    '0x00675[bB]' = '0x3D6B85'; '0x008172' = '0x4F87A5'; '0x00FFD4' = '0x8FD0EA'
}
# numeric constructor triples, anchored on the opening paren
$num = [ordered]@{
    '\(\s*0, ?150, ?136'  = '(79, 135, 165'
    '\(\s*38, ?166, ?154' = '(107, 157, 184'
    '\(\s*0, ?103, ?91\b' = '(61, 107, 133'
    '\(\s*0, ?129, ?114'  = '(79, 135, 165'
    '\(\s*0, ?137, ?123'  = '(63, 111, 138'
}

$targets = @(
    Get-ChildItem "$repo\src\slic3r" -Recurse -Include *.cpp,*.hpp,*.h,*.c
    Get-ChildItem "$repo\resources\images" -Recurse -Include *.svg
    Get-ChildItem "$repo\resources\web" -Recurse -Include *.svg,*.css,*.js,*.html
)

$changed = 0; $hits = 0
foreach ($f in $targets) {
    $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
    $hasBom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
    $text = [System.Text.Encoding]::UTF8.GetString($bytes, ($hasBom ? 3 : 0), $bytes.Length - ($hasBom ? 3 : 0))
    $orig = $text
    foreach ($k in $hex.Keys) {
        if ($f.Name -eq 'BitmapCache.cpp' -and $k -eq '#00AE42') { continue }
        $text = [regex]::Replace($text, $k, $hex[$k], 'IgnoreCase')
    }
    $isCode = $f.Extension -in '.cpp', '.hpp', '.h', '.c'
    if ($isCode) {
        foreach ($k in $int.Keys) { $text = [regex]::Replace($text, $k, $int[$k]) }
        foreach ($k in $num.Keys) { $text = [regex]::Replace($text, $k, $num[$k]) }
    }
    if ($text -ne $orig) {
        $changed++
        if (-not $DryRun) {
            [System.IO.File]::WriteAllText($f.FullName, $text, (New-Object System.Text.UTF8Encoding($hasBom)))
        }
    }
}
"accent sweep: $changed file(s) rewritten, dryrun: $($DryRun.IsPresent)"
