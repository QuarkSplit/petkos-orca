<#
.SYNOPSIS
    Build Podslicer with a progress bar that actually moves.

.DESCRIPTION
    A slicer build is thousands of translation units and MSBuild's own output is either a
    silent wall or a scrolling one. Neither answers the only question a person watching it
    has: how far in am I, and how long is left.

    So this counts. It reads the number of source files out of the .vcxproj it is about to
    build, watches MSBuild name each one as it compiles it, and turns that into a percentage,
    a rate and an estimate that gets better as it goes. Errors are printed the moment they
    appear rather than saved up for the end, because an error at file 40 of 900 should stop
    you waiting for the other 860.

.PARAMETER Target
    MSBuild target: libslic3r, libslic3r_gui, libslic3r_tests, OrcaSlicer, or ALL_BUILD.

.PARAMETER Config
    Release (default) or Debug.

.PARAMETER Quiet
    No progress bar, one line per 10% instead. For logs and unattended runs.

.EXAMPLE
    .\tools\pod-build.ps1 libslic3r_gui
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string] $Target = 'OrcaSlicer',
    [string] $Config = 'Release',
    [string] $BuildDir = '',
    [switch] $Quiet
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $repo 'build' }

if (-not (Test-Path (Join-Path $BuildDir 'CMakeCache.txt'))) {
    Write-Host "No configured build at $BuildDir - run cmake first." -ForegroundColor Red
    exit 1
}

# ---- how much work is this? -------------------------------------------------------------
# The .vcxproj lists every translation unit, so the denominator is known before a single file
# is compiled. Falls back to a guess only if the project cannot be found, and says so.
$proj = Get-ChildItem -Path $BuildDir -Recurse -Filter "$Target.vcxproj" -ErrorAction SilentlyContinue |
        Select-Object -First 1
$total = 0
if ($proj) {
    $total = ([regex]::Matches((Get-Content $proj.FullName -Raw), '<ClCompile\s+Include=')).Count
}
$estimated = $false
if ($total -le 0) { $total = 400; $estimated = $true }

$targetsAll = $Target -in @('OrcaSlicer', 'ALL_BUILD', 'INSTALL')
if ($targetsAll) {
    # A whole-app build compiles its dependencies too, so the denominator is every project's files.
    $total = 0
    Get-ChildItem -Path $BuildDir -Recurse -Filter '*.vcxproj' -ErrorAction SilentlyContinue | ForEach-Object {
        $total += ([regex]::Matches((Get-Content $_.FullName -Raw), '<ClCompile\s+Include=')).Count
    }
    if ($total -le 0) { $total = 3000; $estimated = $true }
}

Write-Host ""
Write-Host "  Podslicer build" -ForegroundColor Cyan
Write-Host "  target   $Target ($Config)"
Write-Host ("  files    {0}{1}" -f $total, $(if ($estimated) { ' (estimated)' } else { '' }))
Write-Host "  note     only files that changed are recompiled, so this usually finishes early"
Write-Host ""

# ---- run it -----------------------------------------------------------------------------
$started   = Get-Date
$done      = 0
$errors    = New-Object System.Collections.Generic.List[string]
$warnings  = 0
$lastPct   = -1
$current   = ''

# -v:n names each source file as the ClCompile task reaches it, which is the only per-file
# signal MSBuild emits. -m parallelises, so the names arrive out of order - the COUNT is the
# progress, never the position.
$args = @(
    '--build', $BuildDir,
    '--config', $Config,
    '--target', $Target,
    '--', '-m', '-v:n', '-nologo'
)

& cmake @args 2>&1 | ForEach-Object {
    $line = [string]$_

    if ($line -match '(?i)\berror\s+[A-Z]+\d+\s*:' -or $line -match '(?i):\s*fatal error\b') {
        $errors.Add($line.Trim())
        if (-not $Quiet) { Write-Progress -Activity 'Building' -Completed }
        Write-Host $line.Trim() -ForegroundColor Red
        return
    }
    if ($line -match '(?i)\bwarning\s+[A-Z]+\d+\s*:') { $warnings++; return }

    # "  Foo.cpp" - the ClCompile task announcing a translation unit.
    if ($line -match '^\s{2,}([A-Za-z0-9_\-\.\+]+\.(cpp|cxx|cc|c))\s*$') {
        $done++
        $current = $Matches[1]

        $elapsed = (Get-Date) - $started
        $pct     = [Math]::Min(99, [int](100 * $done / [Math]::Max(1, $total)))
        $rate    = $done / [Math]::Max(0.5, $elapsed.TotalSeconds)
        $left    = [Math]::Max(0, $total - $done)
        $eta     = if ($rate -gt 0) { [TimeSpan]::FromSeconds($left / $rate) } else { [TimeSpan]::Zero }

        if ($Quiet) {
            if ($pct -ge $lastPct + 10) {
                $lastPct = $pct - ($pct % 10)
                Write-Host ("  {0,3}%  {1}/{2} files  {3:mm\:ss} elapsed" -f $pct, $done, $total, $elapsed)
            }
        }
        else {
            Write-Progress -Activity "Building $Target ($Config)" `
                           -Status  ("{0}/{1} files - {2:mm\:ss} elapsed, about {3:mm\:ss} left" -f $done, $total, $elapsed, $eta) `
                           -CurrentOperation $current `
                           -PercentComplete $pct
        }
    }
}

$code    = $LASTEXITCODE
$elapsed = (Get-Date) - $started
if (-not $Quiet) { Write-Progress -Activity 'Building' -Completed }

Write-Host ""
if ($code -eq 0) {
    Write-Host ("  BUILT   {0} in {1:mm\:ss}, {2} file(s) compiled, {3} warning(s)" -f $Target, $elapsed, $done, $warnings) -ForegroundColor Green
}
else {
    Write-Host ("  FAILED  {0} after {1:mm\:ss}, {2} error(s)" -f $Target, $elapsed, $errors.Count) -ForegroundColor Red
    # The first error is the one that matters; the rest are usually its echo.
    $errors | Select-Object -First 10 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkRed }
    if ($errors.Count -gt 10) { Write-Host ("    ... and {0} more" -f ($errors.Count - 10)) -ForegroundColor DarkRed }
}
Write-Host ""
exit $code
