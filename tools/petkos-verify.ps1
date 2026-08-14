<#
Petko's Orca: does a per-plate setting actually reach that plate, and only that plate?

This is the check the latency work could not make. A frame can be fast while a plate slices with
another plate's settings, and that is exactly what this fork shipped: the Process panel edits the
PROJECT's process preset, a plate reassigned to another printer has been moved onto that printer's
own default process, and so every value typed into the panel reached a preset the plate does not
use - while the panel went on showing the project's preset name as though it had worked. The
screenshot that finally caught it was a person using the app for thirty seconds.

Two questions, both asked of the code that slicing itself reads:

  1. IN MEMORY - set an override on plate 1, and ask PresetBundle::resolve_plate_slicing_config
     what plate 1 and plate 2 would each slice with. Plate 1 must have it. Plate 2 must not.
     PetkosPerfDriver's Scope phase does this and writes its verdict to the app log.

  2. ON DISK - the same override, after a save. bbs_3mf.cpp used to write a plate's config from a
     hand-written whitelist of eight keys, so anything else worked in-session and vanished on
     save, which is a silent yes about settings someone chose. The run saves a project and this
     reads the file back without the app.

    pwsh -Command "& tools/petkos-verify.ps1"
    pwsh -Command "& tools/petkos-verify.ps1 -Keep"    # leave the saved 3mf for inspection
    pwsh -Command "& tools/petkos-verify.ps1 -Fresh"   # re-seed the verification datadir

Exit code is 0 only if both questions answer yes. It runs on its own datadir, so it does NOT
require closing a slicer you already have open.
#>
[CmdletBinding()]
param(
    [int]    $TimeoutSec = 240,
    [switch] $Keep,
    [switch] $Fresh          # rebuild the verification datadir from the live one
)

$ErrorActionPreference = 'Stop'
$repo    = Split-Path -Parent $PSScriptRoot
$exe     = Join-Path $repo 'build\src\Release\orca-slicer.exe'
$live    = Join-Path $repo 'datadir'
$outRoot = Join-Path $repo 'perf-runs'

if (-not (Test-Path $exe)) { throw "No build at $exe - run tools/petkos-dev-build.ps1 first" }

# Which datadir this run uses is decided by whether a slicer is already open.
#
# The live one is preferred, because it is the config that actually resolves: real printer presets,
# real filaments, a parsed app config. A seeded copy looked tidier and was worse - the copied
# PetkosOrca.conf failed to parse, so the app fell back to first-run setup, spent two minutes
# copying the filament library, and never started the scripted run at all. A check that silently
# becomes a first-run wizard is exactly the kind of instrument that lies.
#
# A copy is used only when a slicer IS running, because the app holds one datadir per instance and
# a correctness check must never require closing a session that may hold an unsaved project.
$busy = [bool](Get-Process -Name 'orca-slicer' -ErrorAction SilentlyContinue)
if ($busy) {
    $datadir = Join-Path $repo 'datadir-verify'
    Write-Host '== a slicer is open, so this runs on its own datadir copy' -ForegroundColor Cyan
    if ($Fresh -and (Test-Path $datadir)) { Remove-Item $datadir -Recurse -Force }
    if (-not (Test-Path $datadir)) {
        if (-not (Test-Path $live)) { throw "No datadir at $live to seed the verification copy from" }
        New-Item -ItemType Directory -Force -Path $datadir | Out-Null
        # Everything except the 143 MB of network plugins and the previous runs' logs. system/ and
        # user/ are what make presets resolve; without them the app opens the setup wizard.
        Get-ChildItem $live -Force | Where-Object { $_.Name -notin @('plugins', 'log') } |
            Copy-Item -Destination $datadir -Recurse -Force
    }
} else {
    $datadir = $live
}
$logdir = Join-Path $datadir 'log'
New-Item -ItemType Directory -Force -Path $logdir | Out-Null

New-Item -ItemType Directory -Force -Path $outRoot | Out-Null

$saved = Join-Path $outRoot 'verify-plate-scope.3mf'
$stem  = Join-Path $outRoot 'verify'
Remove-Item $saved, "$stem.csv", "$stem.summary.txt" -ErrorAction SilentlyContinue

# Only the phases the check needs. The orbit and board phases are latency work and cost a minute
# of wall clock each; this is a correctness run, so they are turned down to nothing.
$spec = "plates=2,warmup=10,orbit=10,switch=1,assign=1,board=1,drag=0,pick=0,scope=1,save=$saved,quit=1"

# The log file is named for the moment the app starts, so anything already in the folder belongs to
# a previous run. Remembering them is what stops a stale PASS from a previous build being read as
# this build's verdict - the exact way an instrument lies.
$before = @(Get-ChildItem $logdir -Filter 'debug_*.log*' -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })

$env:PETKOS_PERF        = '1'
$env:PETKOS_PERF_OUT    = $stem
$env:PETKOS_PERF_SCRIPT = $spec
Write-Host "== running: $spec" -ForegroundColor Cyan

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$p  = Start-Process -FilePath $exe -ArgumentList @('--datadir', $datadir) -PassThru
if (-not $p.WaitForExit($TimeoutSec * 1000)) {
    Write-Warning "run did not finish in ${TimeoutSec}s; killing it"
    $p.Kill(); $p.WaitForExit(10000)
}
$sw.Stop()
Remove-Item Env:PETKOS_PERF, Env:PETKOS_PERF_OUT, Env:PETKOS_PERF_SCRIPT -ErrorAction SilentlyContinue
Write-Host "   app exited after $([int]$sw.Elapsed.TotalSeconds)s" -ForegroundColor DarkGray

# ---------------------------------------------------------------- question 1: in memory
$new = @(Get-ChildItem $logdir -Filter 'debug_*.log*' -ErrorAction SilentlyContinue |
         Where-Object { $before -notcontains $_.FullName } | Sort-Object LastWriteTime)
if (-not $new) {
    Write-Host "FAIL  the run wrote no log, so it reached no verdict" -ForegroundColor Red
    Write-Host "      (a killed process never reaches GUI_App::OnExit)" -ForegroundColor DarkGray
    exit 1
}
$lines   = $new | ForEach-Object { Get-Content $_.FullName -ErrorAction SilentlyContinue }
$verdict = $lines | Select-String -Pattern 'SCOPE CHECK|scope check passed' -SimpleMatch:$false

$memoryOk = $false
if (-not $verdict) {
    Write-Host "FAIL  the run produced no scope verdict at all" -ForegroundColor Red
} elseif ($verdict | Where-Object { $_ -match 'SCOPE CHECK (FAILED|SKIPPED)' }) {
    Write-Host "FAIL  in memory:" -ForegroundColor Red
    $verdict | ForEach-Object { Write-Host "      $($_.Line.Trim())" -ForegroundColor Red }
} else {
    $memoryOk = $true
    Write-Host "PASS  in memory: a plate override reaches its own plate and no other" -ForegroundColor Green
    $verdict | ForEach-Object { Write-Host "      $($_.Line.Trim())" -ForegroundColor DarkGray }
}

# ---------------------------------------------------------------- question 2: on disk
$diskOk = $false
if (-not (Test-Path $saved)) {
    Write-Host "FAIL  on disk: the run saved no project to $saved" -ForegroundColor Red
} else {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($saved)
    try {
        # The plate blocks live in model_settings.config, NOT 3dmodel.model. Reading the wrong
        # file made this report a failure it had not actually tested - an instrument with a hole
        # in it, which is worse than none because it is believed.
        $entry = $zip.Entries | Where-Object { $_.FullName -eq 'Metadata/model_settings.config' }
        if (-not $entry) {
            Write-Host "FAIL  on disk: $saved has no Metadata/model_settings.config" -ForegroundColor Red
        } else {
            $reader = New-Object System.IO.StreamReader($entry.Open())
            $xml    = $reader.ReadToEnd(); $reader.Close()
            $keys   = [regex]::Matches($xml, 'plater_plate_config:([a-z0-9_]+)') |
                      ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
            if ($keys -contains 'wall_loops') {
                $diskOk = $true
                Write-Host "PASS  on disk: the override survived the save" -ForegroundColor Green
                Write-Host "      $($keys.Count) plate key(s) written, including wall_loops" -ForegroundColor DarkGray
            } else {
                Write-Host "FAIL  on disk: wall_loops is not in the saved plate config" -ForegroundColor Red
                Write-Host "      keys written: $($keys -join ', ')" -ForegroundColor DarkGray
                Write-Host "      that is the whitelist fault: it worked in the session and was lost on save" -ForegroundColor DarkGray
            }
        }
    } finally { $zip.Dispose() }
    if (-not $Keep) { Remove-Item $saved -ErrorAction SilentlyContinue }
    else { Write-Host "      kept $saved" -ForegroundColor DarkGray }
}

Write-Host ''
if ($memoryOk -and $diskOk) {
    Write-Host 'VERIFIED - per-plate settings reach their plate and survive a save.' -ForegroundColor Green
    exit 0
}
Write-Host 'NOT VERIFIED - see the failures above.' -ForegroundColor Red
exit 1
