<#
Petko's Orca: the build to use while iterating on code.

build_release_vs2022.bat is the correct FULL build and should stay the thing you run before
trusting a result. It is the wrong thing to run twenty times an hour, because it also builds six
test binaries, runs gettext, and then installs ~8,000 resource files - none of which a code change
touches - and it hands MSBuild every core, which is what makes the machine unusable while it runs.

This builds only the app, on cores-to-spare, at below-normal priority.

    pwsh -Command "& tools/petkos-dev-build.ps1"           # the DLL and exe, nothing else
    pwsh -Command "& tools/petkos-dev-build.ps1 -Jobs 4"    # even gentler
    pwsh -Command "& tools/petkos-dev-build.ps1 -Full"      # everything, as the bat does

Two things it deliberately does NOT do:
  - It does not re-run cmake. A changed CMakeLists needs -Configure (or the full bat) once;
    otherwise the configure step is 35-48 s of nothing on every iteration.
  - It does not refresh build/OrcaSlicer/. That staging copy is only needed for a packaged run;
    run-petkos-orca.bat launches build/src/Release/orca-slicer.exe directly, which this updates.
#>
[CmdletBinding()]
param(
    [int]    $Jobs = 0,          # 0 = leave two cores for the desktop
    [switch] $Full,              # ALL_BUILD + install, i.e. what the bat does
    [switch] $Configure,         # re-run cmake first (only needed when CMakeLists changed)
    [string] $Config = 'Release',
    [ValidateSet('Idle','BelowNormal','Normal')]
    [string] $Priority = 'BelowNormal'
)

$ErrorActionPreference = 'Stop'
$repo  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'build'
if (-not (Test-Path $build)) { throw "No build tree at $build - run build_release_vs2022.bat once first" }

if ($Jobs -le 0) {
    $cores = [Environment]::ProcessorCount
    $Jobs  = [Math]::Max(1, $cores - 2)
}

if (Get-Process -Name 'orca-slicer' -ErrorAction SilentlyContinue) {
    # Windows will not overwrite a mapped image, but it will happily rename one within the same
    # volume - the running process keeps its old mapping and the linker writes fresh files.
    # PETKOS-ORCA.md documents the trick; this used to print it as an instruction and then throw,
    # which is a script describing work it is able to do. Killing a running slicer to unblock a
    # link risks a project the user has not saved; renaming risks nothing, so just rename.
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    foreach ($name in @('OrcaSlicer.dll', 'orca-slicer.exe', 'orca-slicer.pdb', 'OrcaSlicer.pdb')) {
        $path = Join-Path $build "src\$Config\$name"
        if (-not (Test-Path $path)) { continue }
        $item = Get-Item $path
        $aside = Join-Path $item.DirectoryName ("{0}.inuse-{1}{2}" -f $item.BaseName, $stamp, $item.Extension)
        try {
            Move-Item -LiteralPath $path -Destination $aside -ErrorAction Stop
            Write-Host "  moved aside (app is running): $($item.Name)" -ForegroundColor DarkYellow
        } catch {
            throw "orca-slicer is running and $name could not be moved aside: $_"
        }
    }
    # The post-build event does `cmake -E rm -rf src\<Config>\python` and re-copies it. The app
    # maps python312.dll from that directory, so the rm fails, the post-build exits 1, and the
    # exe never links - after a clean compile, which reads as a build error that isn't one.
    # A mapped file cannot be deleted but CAN be renamed on the same volume, so the whole
    # directory moves aside and the sweep above reclaims it once nothing maps it.
    $pydir = Join-Path $build "src\$Config\python"
    if (Test-Path $pydir) {
        $pyaside = Join-Path $build ("src\$Config\python.inuse-$stamp")
        try {
            Move-Item -LiteralPath $pydir -Destination $pyaside -ErrorAction Stop
            Write-Host "  moved aside (app is running): python\" -ForegroundColor DarkYellow
        } catch {
            throw "orca-slicer is running and python\ could not be moved aside: $_"
        }
    }
}

# Anything moved aside by an EARLIER run is only deletable once nothing maps it. Sweep them here
# rather than leaving a build directory that grows a 128 MB DLL per iteration.
Get-ChildItem -Path (Join-Path $build "src\$Config") -Filter '*.inuse-*' -ErrorAction SilentlyContinue |
    ForEach-Object { try { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction Stop } catch { } }

if ($Configure) {
    Write-Host "cmake configure..." -ForegroundColor Cyan
    Push-Location $build
    & cmake .. -G "Visual Studio 17 2022" -A x64 -DORCA_TOOLS=ON -DCMAKE_BUILD_TYPE=$Config
    Pop-Location
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
}

# orca-slicer.exe is NOT part of the OrcaSlicer target - that one builds OrcaSlicer.dll. The exe
# is a 300 KB launcher from the separate OrcaSlicer_app_gui target, so building 'OrcaSlicer' alone
# leaves whatever exe was already on disk. That was invisible until the app was running and the
# exe had been moved aside, at which point the build produced a fresh DLL and no exe to load it.
$targets = if ($Full) { @('ALL_BUILD') } else { @('OrcaSlicer', 'OrcaSlicer_app_gui') }
$target  = $targets -join ', '
Write-Host "building target $target, config $Config, $Jobs of $([Environment]::ProcessorCount) cores, priority $Priority" -ForegroundColor Cyan

$log = Join-Path $env:TEMP ("petkos-dev-build-{0}.log" -f $Config)
$sw  = [System.Diagnostics.Stopwatch]::StartNew()

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName  = 'cmake'
$psi.Arguments = "--build `"$build`" --config $Config --target $($targets -join ' ') -- -m:$Jobs -clp:ErrorsOnly;Summary"
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError  = $true

$p = [System.Diagnostics.Process]::Start($psi)
try { $p.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::$Priority } catch { }
# BOTH pipes have to be drained AT ONCE. Reading stdout to the end and only then reading stderr
# deadlocks the moment the child writes more to stderr than the pipe buffer holds: the child
# blocks writing, this script blocks reading the other stream, and neither ever moves. It looks
# exactly like a slow build - a live cmake.exe with no compilers under it and no output - and the
# case that triggers it is the common one, because a changed CMakeLists makes MSBuild re-run the
# whole configure and that is thousands of lines. Async on both, then wait.
$outTask = $p.StandardOutput.ReadToEndAsync()
$errTask = $p.StandardError.ReadToEndAsync()
$p.WaitForExit()
$out = $outTask.Result + $errTask.Result
$sw.Stop()
$out | Out-File -FilePath $log -Encoding utf8

$errors = Select-String -Path $log -Pattern 'error [A-Z]+[0-9]+' -AllMatches
if ($p.ExitCode -ne 0 -or $errors) {
    Write-Host "FAILED in $([int]$sw.Elapsed.TotalSeconds)s - $log" -ForegroundColor Red
    $errors | Select-Object -First 20 | ForEach-Object { $_.Line }
    exit 1
}

Write-Host ("OK in {0}m {1:d2}s" -f [int]$sw.Elapsed.TotalMinutes, $sw.Elapsed.Seconds) -ForegroundColor Green
$missing = @()
foreach ($name in @('OrcaSlicer.dll', 'orca-slicer.exe')) {
    $artifact = Join-Path $build "src\$Config\$name"
    if (Test-Path $artifact) {
        Write-Host ("  {0}  {1}" -f (Get-Item $artifact).LastWriteTime.ToString('HH:mm:ss'), $artifact)
    } else {
        $missing += $name
    }
}
# A green build that produced no runnable app is the failure this reports. MSBuild is happy to
# succeed at a target whose output nothing asked for.
if ($missing) { Write-Host ("  MISSING: {0}" -f ($missing -join ', ')) -ForegroundColor Red; exit 1 }
