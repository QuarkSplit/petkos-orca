<#
Petko's Orca: does a downloaded project survive being moved onto a printer this farm owns?

This is THE workflow. The farm has no Bambu machine - it has an Anycubic Kobra S1, a Creality K2
Pro, a Creality SPARKX i7, an Elegoo Centauri Carbon, two Flashforges and a Prusa CORE One - and
the well-plated, coloured, split projects worth downloading are overwhelmingly authored for Bambu.
So every download is a cross-printer import, and the fork's rule for it is that a change of machine
is a TRANSLATION, never a discard: a preset NAME means nothing across machines, but its VALUES
mostly do. Wall count, infill pattern and density, wipe and print order are the same decision on
any FDM machine, and losing them because the machine changed is not a missing feature.

So this run asks the only question that settles it:

    open a real Bambu-authored 3MF -> retarget its plate to a machine we own -> slice it ->
    read the emitted G-code and check the values the project chose are still there.

Every check inside the app is the app marking its own homework. On 2026-08-14 all of them passed
on a build that quoted 21h03m for a 6h45m plate and reported 0.00 g, because a plate pinned to a
placeholder preset is a COMPLETE plate - it is just pinned to the wrong thing. The G-code is the
only artefact that cannot be wrong about what was used, because it is what gets made.

    pwsh -Command "& tools/petkos-import-check.ps1"
    pwsh -Command "& tools/petkos-import-check.ps1 -Printer 'Creality K2 Pro'"
    pwsh -Command "& tools/petkos-import-check.ps1 -Source 'D:\Downloads\thing.3mf' -Keep"

Exit code is 0 only if the plate landed on the requested machine AND the project's own process
values came with it.
#>
[CmdletBinding()]
param(
    [string] $Source  = 'E:\3D-Printing\Projects\Comic Con 2026\01-source\1607565-las-58-talon-v2-helldivers-2\energy_revolver_-_part_1.3mf',
    [string] $Printer = 'Elegoo Centauri Carbon 0.4 nozzle',
    [int]    $TimeoutSec = 900,
    [switch] $Keep,
    [switch] $Fresh
)

$ErrorActionPreference = 'Stop'
$repo    = Split-Path -Parent $PSScriptRoot
$exe     = Join-Path $repo 'build\src\Release\orca-slicer.exe'
$live    = Join-Path $repo 'datadir'
$outRoot = Join-Path $repo 'perf-runs'
$checker = Join-Path $PSScriptRoot 'petkos-gcode-check.py'

if (-not (Test-Path $exe))    { throw "No build at $exe - run tools/petkos-dev-build.ps1 first" }
if (-not (Test-Path $Source)) { throw "No source project at $Source" }

# ---------------------------------------------------------------- what the file asked for
# Read straight out of the 3MF, without the app. This is the project's own declaration, and it is
# what the run has to still be true to afterwards.
$declared = & python -c @"
import json, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
cfg = json.loads(z.read('Metadata/project_settings.config').decode('utf-8'))
keep = ['printer_settings_id','print_settings_id','filament_settings_id','printer_model',
        'filament_type','filament_max_volumetric_speed','wall_loops','sparse_infill_density',
        'sparse_infill_pattern','layer_height']
print(json.dumps({k: cfg[k] for k in keep if k in cfg}))
"@ $Source | ConvertFrom-Json

# A fixture this check cannot read must be a failure, not a vacuous pass: with nothing declared,
# the carried-values loop below would assert nothing and still report VERIFIED.
if ($null -eq $declared -or -not $declared.PSObject.Properties.Name) {
    throw "could not read Metadata/project_settings.config out of $Source - nothing to verify against"
}
foreach ($k in 'wall_loops', 'sparse_infill_density', 'sparse_infill_pattern') {
    if (-not $declared.PSObject.Properties.Name.Contains($k)) {
        throw "the fixture declares no $k; this check would pass vacuously"
    }
}

Write-Host "== the project declares" -ForegroundColor Cyan
foreach ($p in $declared.PSObject.Properties) {
    Write-Host ("   {0,-32} {1}" -f $p.Name, ($p.Value -join ', ')) -ForegroundColor DarkGray
}
Write-Host "== retargeting to '$Printer'" -ForegroundColor Cyan

# ---------------------------------------------------------------- the run
# Same datadir rule as petkos-verify.ps1: prefer the live one because it is the config that
# actually resolves, and fall back to a copy only when a slicer is already open, because a
# correctness check must never require closing a session holding an unsaved project.
$busy = [bool](Get-Process -Name 'orca-slicer' -ErrorAction SilentlyContinue)
if ($busy) {
    $datadir = Join-Path $repo 'datadir-verify'
    Write-Host '== a slicer is open, so this runs on its own datadir copy' -ForegroundColor Cyan
    if ($Fresh -and (Test-Path $datadir)) { Remove-Item $datadir -Recurse -Force }
    if (-not (Test-Path $datadir)) {
        New-Item -ItemType Directory -Force -Path $datadir | Out-Null
        Get-ChildItem $live -Force | Where-Object { $_.Name -notin @('plugins', 'log') } |
            Copy-Item -Destination $datadir -Recurse -Force
    }
} else {
    $datadir = $live
}
$logdir = Join-Path $datadir 'log'
New-Item -ItemType Directory -Force -Path $logdir, $outRoot | Out-Null

$gcode = Join-Path $outRoot 'import-check-plate-1.gcode'
$stem  = Join-Path $outRoot 'import-check'
Remove-Item $gcode, "$stem.csv", "$stem.summary.txt" -ErrorAction SilentlyContinue

# plates=0 means "use the plates the project brought". Everything that is latency measurement is
# turned off; this is a correctness run.
$spec = "plates=0,warmup=25,orbit=0,switch=0,assign=1,printer=$Printer,board=0,drag=0,pick=0,scope=0,context=1,gcode=$gcode,quit=1"

$before = @(Get-ChildItem $logdir -Filter 'debug_*.log*' -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -notlike 'debug_network_*' } | ForEach-Object { $_.FullName })

# A leftover driver env var would install a second idle-driver or fire an extra assignment onto
# this run's fixture; clear them before the launch rather than trusting the previous run's cleanup.
Remove-Item Env:PETKOS_ACCEPT, Env:PETKOS_TEST_ASSIGN -ErrorAction SilentlyContinue

$env:PETKOS_PERF        = '1'
$env:PETKOS_PERF_OUT    = $stem
$env:PETKOS_PERF_SCRIPT = $spec
Write-Host "== running: $spec" -ForegroundColor Cyan

$sw = [System.Diagnostics.Stopwatch]::StartNew()
try {
    $p = Start-Process -FilePath $exe -ArgumentList @('--datadir', "`"$datadir`"", "`"$Source`"") -PassThru
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        Write-Warning "run did not finish in ${TimeoutSec}s; killing it"
        $p.Kill(); $p.WaitForExit(10000)
    }
} finally {
    Remove-Item Env:PETKOS_PERF, Env:PETKOS_PERF_OUT, Env:PETKOS_PERF_SCRIPT -ErrorAction SilentlyContinue
}
$sw.Stop()
Write-Host "   app exited after $([int]$sw.Elapsed.TotalSeconds)s" -ForegroundColor DarkGray

# ---------------------------------------------------------------- what the app said about itself
$new = @(Get-ChildItem $logdir -Filter 'debug_*.log*' -ErrorAction SilentlyContinue |
         Where-Object { $_.Name -notlike 'debug_network_*' -and $before -notcontains $_.FullName } |
         Sort-Object LastWriteTime)
$lines = $new | ForEach-Object { Get-Content $_.FullName -ErrorAction SilentlyContinue }
$lines | Select-String -Pattern 'CONTEXT CHECK|context check passed|completed the context of|carrying \d+ chosen|SLICE FAILED' |
    ForEach-Object { Write-Host "   $($_.Line.Trim())" -ForegroundColor DarkGray }
# An in-app check failure must gate the verdict, not just scroll past in grey.
$ctxBad = @($lines | Select-String -Pattern 'CONTEXT CHECK (FAILED|SKIPPED)|SLICE FAILED')

# ---------------------------------------------------------------- what it actually made
if (-not (Test-Path $gcode)) {
    Write-Host ''
    Write-Host "FAIL  the run emitted no G-code, so nothing here is verified" -ForegroundColor Red
    Write-Host "      (a killed process never reaches the slice phase; check the log above)" -ForegroundColor DarkGray
    exit 1
}

Write-Host ''
& python $checker $gcode --expect-printer $Printer --expect-filament PETG --min-flow 8
$checkOk = ($LASTEXITCODE -eq 0)

# ---------------------------------------------------------------- did the CHOICES come with it
# The heart of it. A process preset's name is machine tuning and is expected to change; the values
# somebody chose are not, and these three are the ones the owner calls trivial-to-translate and
# inexcusable to lose.
$carried = & python -c @"
import json, re, sys
want = json.loads(sys.argv[2])
keys = ['wall_loops','sparse_infill_density','sparse_infill_pattern']
got = {}
with open(sys.argv[1], 'r', encoding='utf-8', errors='replace') as fh:
    for line in fh:
        if not line.startswith(';'):
            continue
        m = re.match(r'^;\s*([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$', line)
        if m and m.group(1) in keys:
            got[m.group(1)] = m.group(2)
out = []
for k in keys:
    if k in want:
        out.append({'key': k, 'wanted': str(want[k]), 'got': got.get(k), 'ok': got.get(k) == str(want[k])})
print(json.dumps(out))
"@ $gcode ($declared | ConvertTo-Json -Compress) | ConvertFrom-Json

Write-Host ''
Write-Host "== did the project's own choices survive the machine change" -ForegroundColor Cyan
$carriedOk = $true
foreach ($c in $carried) {
    if ($c.ok) {
        Write-Host ("   PASS  {0,-24} {1}" -f $c.key, $c.got) -ForegroundColor Green
    } else {
        $carriedOk = $false
        Write-Host ("   FAIL  {0,-24} the project chose {1}, the slice used {2}" -f $c.key, $c.wanted, $c.got) -ForegroundColor Red
    }
}
if (-not $carried) {
    Write-Host "   (the project declared none of these, so there was nothing to carry)" -ForegroundColor DarkGray
}

if (-not $Keep) { Remove-Item $gcode -ErrorAction SilentlyContinue }
else { Write-Host "`n   kept $gcode" -ForegroundColor DarkGray }

Write-Host ''
if ($ctxBad) {
    Write-Host "FAIL  the app's own checks failed during the run:" -ForegroundColor Red
    $ctxBad | ForEach-Object { Write-Host "      $($_.Line.Trim())" -ForegroundColor Red }
}
if ($checkOk -and $carriedOk -and -not $ctxBad) {
    Write-Host "VERIFIED - the project moved to '$Printer' and brought its own settings with it." -ForegroundColor Green
    exit 0
}
Write-Host 'NOT VERIFIED - see the failures above.' -ForegroundColor Red
exit 1
