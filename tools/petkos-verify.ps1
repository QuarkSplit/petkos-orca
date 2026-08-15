<#
Petko's Orca: does a per-plate setting actually reach that plate, and only that plate?

This is the check the latency work could not make. A frame can be fast while a plate slices with
another plate's settings, and that is exactly what this fork shipped: the Process panel edits the
PROJECT's process preset, a plate reassigned to another printer has been moved onto that printer's
own default process, and so every value typed into the panel reached a preset the plate does not
use - while the panel went on showing the project's preset name as though it had worked. The
screenshot that finally caught it was a person using the app for thirty seconds.

Four questions. The first three are asked of the code that slicing itself reads; the fourth is
asked of the G-code, because the first three all passed on the build that quoted 21h03m for a
6h45m plate.

  1. IN MEMORY - set an override on plate 1, and ask PresetBundle::resolve_plate_slicing_config
     what plate 1 and plate 2 would each slice with. Plate 1 must have it. Plate 2 must not.
     PetkosPerfDriver's Scope phase does this and writes its verdict to the app log.

  2. ON DISK - the same override, after a save. bbs_3mf.cpp used to write a plate's config from a
     hand-written whitelist of eight keys, so anything else worked in-session and vanished on
     save, which is a silent yes about settings someone chose. The run saves a project and this
     reads the file back without the app.

  3. IS THE PROJECT PRINTER GONE - the check the per-plate architecture rests on. Every plate must
     carry a complete context of its own; an EMPTY context must refuse to resolve, because while
     it resolved by reading the globally selected preset every plate had that preset standing
     behind it; and moving the global selection must not change what a plate slices with. The
     driver's Context phase asks all three and writes its verdict to the app log, and this run
     also confirms each plate named its own printer in the saved file.

    pwsh -Command "& tools/petkos-verify.ps1"
    pwsh -Command "& tools/petkos-verify.ps1 -Keep"    # leave the saved 3mf for inspection
    pwsh -Command "& tools/petkos-verify.ps1 -Fresh"   # re-seed the verification datadir

  4. IN THE OUTPUT - the run slices plate 1 and this reads the emitted G-code with
     tools/petkos-gcode-check.py. printer_settings_id, print_settings_id, filament_settings_id and
     filament_max_volumetric_speed are what the slice ACTUALLY used, and they are the only
     statement about it that cannot be wrong, because the G-code is what gets made. This is the
     question that would have caught the placeholder-preset incident on 2026-08-14, and the only
     one that did.

Exit code is 0 only if all four questions answer yes. It runs on its own datadir, so it does NOT
require closing a slicer you already have open.
#>
[CmdletBinding()]
param(
    [int]    $TimeoutSec = 600,   # a real slice happens in here now; see question 4
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
    Write-Host '== a slicer is open, so this runs on its own datadir copy' -ForegroundColor Cyan
    # pod clone is the ONE clone implementation: copy minus plugins/log, plus the conf edit
    # that stops it CLAIMING networking is installed - that claim beside an empty plugins
    # dir is a modal download dialog in post_init, which hangs an unattended run.
    $cloneArgs = @('clone'); if ($Fresh) { $cloneArgs += '--fresh' }
    $datadir = (& python (Join-Path $PSScriptRoot 'pod.py') @cloneArgs | Select-Object -Last 1).Trim()
    if (-not (Test-Path $datadir)) { throw "pod clone did not produce a datadir (got '$datadir')" }
} else {
    $datadir = $live
}
$logdir = Join-Path $datadir 'log'
New-Item -ItemType Directory -Force -Path $logdir | Out-Null

New-Item -ItemType Directory -Force -Path $outRoot | Out-Null

$saved = Join-Path $outRoot 'verify-plate-scope.3mf'
$gcode = Join-Path $outRoot 'verify-plate-1.gcode'
$stem  = Join-Path $outRoot 'verify'
Remove-Item $saved, $gcode, "$stem.csv", "$stem.summary.txt" -ErrorAction SilentlyContinue

# Only the phases the check needs. The orbit and board phases are latency work and cost a minute
# of wall clock each; this is a correctness run, so they are turned down to nothing.
# A real slice is the expensive part of this run and it is the point of it: gcode= is what makes
# question 4 possible. The timeout below has to allow for it.
$spec = "plates=2,warmup=10,orbit=10,switch=1,assign=1,board=1,drag=0,pick=0,scope=1,context=1,save=$saved,gcode=$gcode,quit=1"

# The log file is named for the moment the app starts, so anything already in the folder belongs to
# a previous run. Remembering them is what stops a stale PASS from a previous build being read as
# this build's verdict - the exact way an instrument lies.
# debug_[A-Z]* excludes the encrypted debug_network_*.log.enc files, which would otherwise let a
# crash-before-the-main-log-opens still satisfy the "wrote a log" gate below.
$before = @(Get-ChildItem $logdir -Filter 'debug_*.log*' -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -notlike 'debug_network_*' } | ForEach-Object { $_.FullName })

# A leftover driver env var would install a second idle-driver or fire an extra assignment;
# clear them rather than trusting the previous run's cleanup.
Remove-Item Env:PETKOS_ACCEPT, Env:PETKOS_TEST_ASSIGN -ErrorAction SilentlyContinue

$env:PETKOS_PERF        = '1'
$env:PETKOS_PERF_OUT    = $stem
$env:PETKOS_PERF_SCRIPT = $spec
Write-Host "== running: $spec" -ForegroundColor Cyan

$sw = [System.Diagnostics.Stopwatch]::StartNew()
try {
    $p = Start-Process -FilePath $exe -ArgumentList @('--datadir', "`"$datadir`"") -PassThru
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        Write-Warning "run did not finish in ${TimeoutSec}s; killing it"
        $p.Kill(); $p.WaitForExit(10000)
    }
} finally {
    Remove-Item Env:PETKOS_PERF, Env:PETKOS_PERF_OUT, Env:PETKOS_PERF_SCRIPT -ErrorAction SilentlyContinue
}
$sw.Stop()
Write-Host "   app exited after $([int]$sw.Elapsed.TotalSeconds)s" -ForegroundColor DarkGray

# ---------------------------------------------------------------- question 1: in memory
$new = @(Get-ChildItem $logdir -Filter 'debug_*.log*' -ErrorAction SilentlyContinue |
         Where-Object { $_.Name -notlike 'debug_network_*' -and $before -notcontains $_.FullName } |
         Sort-Object LastWriteTime)
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

# ------------------------------------------------- question 3a: the project printer is gone
$contextVerdict = $lines | Select-String -Pattern 'CONTEXT CHECK|context check passed'
$contextOk = $false
if (-not $contextVerdict) {
    Write-Host "FAIL  the run produced no context verdict at all" -ForegroundColor Red
} elseif ($contextVerdict | Where-Object { $_ -match 'CONTEXT CHECK (FAILED|SKIPPED)' }) {
    Write-Host "FAIL  the project printer is still reachable:" -ForegroundColor Red
    $contextVerdict | ForEach-Object { Write-Host "      $($_.Line.Trim())" -ForegroundColor Red }
} else {
    $contextOk = $true
    Write-Host "PASS  every plate owns its context, and the global selection reaches none of them" -ForegroundColor Green
    $contextVerdict | ForEach-Object { Write-Host "      $($_.Line.Trim())" -ForegroundColor DarkGray }
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
            # Question 3b, on the same file: each plate must have named its own printer. A
            # migrated project writes one per plate, so a file with fewer of these than it has
            # plates is a plate that left the app with no machine of its own.
            $printers = [regex]::Matches($xml, 'key="plater_printer_preset" value="([^"]*)"') |
                        ForEach-Object { $_.Groups[1].Value }
            $plateCount = ([regex]::Matches($xml, '<plate>')).Count
            if ($printers.Count -ge $plateCount -and $plateCount -gt 0 -and
                -not ($printers | Where-Object { [string]::IsNullOrWhiteSpace($_) })) {
                Write-Host "PASS  on disk: all $plateCount plate(s) named their own printer" -ForegroundColor Green
                Write-Host "      $($printers -join ' | ')" -ForegroundColor DarkGray
            } else {
                $contextOk = $false
                Write-Host "FAIL  on disk: $($printers.Count) printer name(s) written for $plateCount plate(s)" -ForegroundColor Red
            }

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

# --------------------------------------------------- question 4: in the emitted G-code
$gcodeOk = $false
if (-not (Test-Path $gcode)) {
    Write-Host "FAIL  in the output: the run sliced no G-code to $gcode" -ForegroundColor Red
    Write-Host "      (without it, every check above is the app marking its own homework)" -ForegroundColor DarkGray
} else {
    $checker = Join-Path $PSScriptRoot 'petkos-gcode-check.py'
    $out = & python $checker $gcode 2>&1
    if ($LASTEXITCODE -eq 0) {
        $gcodeOk = $true
        Write-Host "PASS  in the output: the slice names real presets and produced real figures" -ForegroundColor Green
    } else {
        Write-Host "FAIL  in the output:" -ForegroundColor Red
    }
    $out | ForEach-Object { Write-Host "      $_" -ForegroundColor DarkGray }
    if (-not $Keep) { Remove-Item $gcode -ErrorAction SilentlyContinue }
    else { Write-Host "      kept $gcode" -ForegroundColor DarkGray }
}

Write-Host ''
if ($memoryOk -and $diskOk -and $contextOk -and $gcodeOk) {
    Write-Host 'VERIFIED - every plate owns its context, its settings reach it alone, both survive a save, and the G-code agrees.' -ForegroundColor Green
    exit 0
}
Write-Host 'NOT VERIFIED - see the failures above.' -ForegroundColor Red
exit 1
