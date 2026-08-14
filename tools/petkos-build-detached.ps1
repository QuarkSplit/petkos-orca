<#
Petko's Orca: start a build that outlives the session that started it.

    pwsh -Command "& tools/petkos-build-detached.ps1"          # start it, return immediately
    pwsh -Command "& tools/petkos-build-detached.ps1 -Wait"    # start it and block until done

Why this exists. A full rebuild is 30-35 minutes, and a build launched from an agent's shell is a
child of that shell. When the shell is stopped - and a long-running one reliably is - the top-level
cmake dies with it. MSBuild's worker nodes keep compiling for a minute or two, so the build tree
fills with fresh object files and then goes silent, having never reached the link step. The DLL on
disk is the previous one. Nothing reports an error, because the thing that would have reported it
is the process that was killed. That is indistinguishable from a finished build unless you check
the artefact timestamps, and it cost two cycles before it was understood.

Task Scheduler owns its own processes, so a task started here belongs to the service rather than to
whatever launched it, and it runs to completion or fails honestly.

Completion is signalled by a file rather than by a process handle, for the same reason: a handle
belongs to a session and a file does not.
    $env:TEMP\petkos-build-detached.done   contains the exit code, written last
    $env:TEMP\petkos-build-detached.log    the full build output
#>
[CmdletBinding()]
param(
    [switch] $Wait,
    [int]    $TimeoutSec = 3600,
    [string] $TaskName = 'PetkosOrcaBuild'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$cmd  = Join-Path $PSScriptRoot 'petkos-build-detached.cmd'
$done = Join-Path $env:TEMP 'petkos-build-detached.done'
$log  = Join-Path $env:TEMP 'petkos-build-detached.log'

if (-not (Test-Path $cmd)) { throw "Missing $cmd" }
Remove-Item $done -ErrorAction SilentlyContinue

# Re-register every time: the script may have moved, and a stale task definition pointing at an old
# path would run silently and write nothing, which is the failure this whole file exists to avoid.
& schtasks /Create /TN $TaskName /TR "`"$cmd`"" /SC ONCE /ST 00:00 /F /RL LIMITED | Out-Null
if ($LASTEXITCODE -ne 0) { throw "could not register the build task (schtasks rc=$LASTEXITCODE)" }

& schtasks /Run /TN $TaskName | Out-Null
if ($LASTEXITCODE -ne 0) { throw "could not start the build task (schtasks rc=$LASTEXITCODE)" }
Write-Host "build task '$TaskName' started; log: $log" -ForegroundColor Cyan

if (-not $Wait) { return }

$sw = [System.Diagnostics.Stopwatch]::StartNew()
while (-not (Test-Path $done)) {
    if ($sw.Elapsed.TotalSeconds -gt $TimeoutSec) { throw "build did not finish in ${TimeoutSec}s; see $log" }
    Start-Sleep -Seconds 10
}
$rc = (Get-Content $done -Raw).Trim()
Write-Host ("build finished in {0}m {1:d2}s, rc={2}" -f [int]$sw.Elapsed.TotalMinutes, $sw.Elapsed.Seconds, $rc) `
    -ForegroundColor $(if ($rc -eq '0') { 'Green' } else { 'Red' })
if ($rc -ne '0') {
    Select-String -Path $log -Pattern 'error [A-Z]+[0-9]+' -AllMatches |
        Select-Object -First 20 | ForEach-Object { $_.Line }
    exit 1
}
foreach ($name in @('OrcaSlicer.dll', 'orca-slicer.exe')) {
    $a = Join-Path $repo "build\src\Release\$name"
    if (Test-Path $a) { Write-Host ("  {0}  {1}" -f (Get-Item $a).LastWriteTime.ToString('HH:mm:ss'), $a) }
    else { Write-Host "  MISSING: $name" -ForegroundColor Red; exit 1 }
}
