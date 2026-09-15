<#
Podslicer: does a second project ADD to an open one, whole?

Opens project A, adds project B through Plater::add_project (the same door a dropped 3MF takes
on a non-blank project), and reads the perf driver's ADD CHECK and CONTEXT CHECK verdicts out
of the app's own log. Then writes the merged project and inspects it, so the file on disk is
checked as well as the memory. Runs the pair in both orders, because "A then B" and "B then A"
exercise different pools and different foreign machines.

    pwsh -Command "& tools/pod-add-check.ps1"
    pwsh -Command "& tools/pod-add-check.ps1 -A path\a.3mf -B path\b.3mf"

Exit code 0 when every verdict passed.
#>
param(
    [string] $A = 'E:\3D-Printing\Projects\Comic Con 2026\01-source\1035578-star-wars-keychain-logo-empire-rebellion-ams\3MF+-+Star+Wars+logo.3mf',
    [string] $B = 'E:\3D-Printing\Projects\3D WiPs\LAS-16+Sickle.3mf',
    [string] $Out = 'D:\Dev\petkos-orca\perf-runs\add-check'
)
$ErrorActionPreference = 'Stop'
$repo = 'D:\Dev\petkos-orca'
New-Item -ItemType Directory -Force $Out | Out-Null
$failed = 0

function Run-Add([string]$first, [string]$second, [string]$tag) {
    $save = Join-Path $Out "$tag.3mf"
    if (Test-Path $save) { Remove-Item $save -Force }
    $spec = "plates=0,warmup=10,orbit=0,switch=0,assign=0,board=0,drag=0,pick=0,scope=0,context=1,add=$second,save=$save,quit=1"
    Write-Host "== $tag : open '$first', add '$second'"
    $text = & python (Join-Path $repo 'tools\pod.py') run --project $first --timeout 600 $spec 2>&1 | Out-String
    Write-Host $text
    $ok = ($text -match 'ADD CHECK passed') -and ($text -match 'CONTEXT CHECK passed') -and (Test-Path $save)
    if (-not $ok) { $script:failed++ ; Write-Host "!! $tag FAILED" -ForegroundColor Red; return }
    Write-Host "-- $tag merged file:"
    & python (Join-Path $repo 'tools\pod.py') inspect $save 2>&1 | Select-Object -First 40 | ForEach-Object { Write-Host "   $_" }
}

Run-Add $A $B 'a-then-b'
Run-Add $B $A 'b-then-a'
Run-Add $A $A 'a-twice'

if ($failed -gt 0) { Write-Host "pod-add-check: $failed run(s) FAILED" -ForegroundColor Red; exit 1 }
Write-Host "pod-add-check: every run passed" -ForegroundColor Green
exit 0
