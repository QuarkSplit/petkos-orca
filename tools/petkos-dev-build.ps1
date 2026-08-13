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
    # Windows will not overwrite a mapped image, but it will happily rename one. PETKOS-ORCA.md
    # documents this: killing a running slicer to unblock a link risks an unsaved project,
    # renaming risks nothing.
    Write-Warning 'orca-slicer is running. Close it, or move the DLL aside first:'
    Write-Warning '  Move-Item build\src\Release\OrcaSlicer.dll build\src\Release\OrcaSlicer.inuse.dll'
    throw 'refusing to link over a running app'
}

if ($Configure) {
    Write-Host "cmake configure..." -ForegroundColor Cyan
    Push-Location $build
    & cmake .. -G "Visual Studio 17 2022" -A x64 -DORCA_TOOLS=ON -DCMAKE_BUILD_TYPE=$Config
    Pop-Location
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
}

$target = if ($Full) { 'ALL_BUILD' } else { 'OrcaSlicer' }
Write-Host "building target $target, config $Config, $Jobs of $([Environment]::ProcessorCount) cores, priority $Priority" -ForegroundColor Cyan

$log = Join-Path $env:TEMP ("petkos-dev-build-{0}.log" -f $Config)
$sw  = [System.Diagnostics.Stopwatch]::StartNew()

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName  = 'cmake'
$psi.Arguments = "--build `"$build`" --config $Config --target $target -- -m:$Jobs -clp:ErrorsOnly;Summary"
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError  = $true

$p = [System.Diagnostics.Process]::Start($psi)
try { $p.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::$Priority } catch { }
$out = $p.StandardOutput.ReadToEnd() + $p.StandardError.ReadToEnd()
$p.WaitForExit()
$sw.Stop()
$out | Out-File -FilePath $log -Encoding utf8

$errors = Select-String -Path $log -Pattern 'error [A-Z]+[0-9]+' -AllMatches
if ($p.ExitCode -ne 0 -or $errors) {
    Write-Host "FAILED in $([int]$sw.Elapsed.TotalSeconds)s - $log" -ForegroundColor Red
    $errors | Select-Object -First 20 | ForEach-Object { $_.Line }
    exit 1
}

$dll = Join-Path $build 'src\Release\OrcaSlicer.dll'
Write-Host ("OK in {0}m {1:d2}s" -f [int]$sw.Elapsed.TotalMinutes, $sw.Elapsed.Seconds) -ForegroundColor Green
if (Test-Path $dll) { Write-Host ("  {0}  {1}" -f (Get-Item $dll).LastWriteTime.ToString('HH:mm:ss'), $dll) }
