<#
Petko's Orca: take the GUI latency numbers at several plate counts, unattended.

Carried in the repo rather than a scratch folder because a performance claim that cannot be
re-run is a story. Each pass launches the real app through its isolated datadir, drives the
scripted run in PetkosPerfDriver, and leaves a CSV and a summary per plate count.

    pwsh -Command "& tools/petkos-perf-run.ps1"                        # 1, 6 and 36 plates
    pwsh -Command "& tools/petkos-perf-run.ps1 -Plates 36 -Tag after"  # one pass, labelled

Call it with -Command, not -File: under -File every argument arrives as a string, so
-Plates 1,6,36 is read as the single number 1636 and the run asks for 1,636 plates.

The app must not already be running: it holds the DLL, and two instances share one datadir.
#>
[CmdletBinding()]
param(
    [int[]]  $Plates = @(1, 6, 36),   # 36 is MAX_PLATE_COUNT, the most the list will hold
    [string] $Tag = 'baseline',
    [int]    $Orbit = 600,
    [int]    $Warmup = 60,
    [int]    $Switches = 10,
    [int]    $Assigns = 4,
    [int]    $Board = 200,
    [int]    $Drags = 5,
    [int]    $TimeoutSec = 300,
    [string] $Project = ''
)

$ErrorActionPreference = 'Stop'
$repo    = Split-Path -Parent $PSScriptRoot
$exe     = Join-Path $repo 'build\src\Release\orca-slicer.exe'
$datadir = Join-Path $repo 'datadir'
$outRoot = Join-Path $repo 'perf-runs'

if (-not (Test-Path $exe)) { throw "No build at $exe - run build_release_vs2022.bat slicer first" }
if (Get-Process -Name 'orca-slicer' -ErrorAction SilentlyContinue) {
    throw 'orca-slicer is already running; close it first (one datadir, one instance)'
}
New-Item -ItemType Directory -Force -Path $outRoot | Out-Null

$results = @()
foreach ($n in $Plates) {
    $stem = Join-Path $outRoot "$Tag-$n"
    Remove-Item "$stem.csv", "$stem.summary.txt" -ErrorAction SilentlyContinue

    $spec = "plates=$n,warmup=$Warmup,orbit=$Orbit,switch=$Switches,assign=$Assigns,board=$Board,drag=$Drags,quit=1"
    Write-Host "== $n plate(s): $spec" -ForegroundColor Cyan

    $env:PETKOS_PERF        = '1'
    $env:PETKOS_PERF_OUT    = $stem
    $env:PETKOS_PERF_SCRIPT = $spec

    $args = @('--datadir', $datadir)
    if ($Project) { $args += $Project }

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p  = Start-Process -FilePath $exe -ArgumentList $args -PassThru
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        Write-Warning "run did not finish in ${TimeoutSec}s; killing it. NOTE: a killed process never reaches GUI_App::OnExit, so this pass has no CSV."
        $p.Kill()
        $p.WaitForExit(10000)
    }
    $sw.Stop()

    Remove-Item Env:PETKOS_PERF, Env:PETKOS_PERF_OUT, Env:PETKOS_PERF_SCRIPT -ErrorAction SilentlyContinue

    if (Test-Path "$stem.summary.txt") {
        Write-Host "   wrote $stem.summary.txt  (wall $([int]$sw.Elapsed.TotalSeconds)s)" -ForegroundColor Green
        $results += [pscustomobject]@{ Plates = $n; Summary = "$stem.summary.txt" }
    } else {
        Write-Warning "   no summary for $n plates - the run produced nothing"
    }
}

Write-Host ''
foreach ($r in $results) {
    Write-Host ("=" * 78)
    Write-Host "PLATES = $($r.Plates)" -ForegroundColor Yellow
    Get-Content $r.Summary
}
