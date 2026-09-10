<#
Petko's Orca: the build to use while iterating on code.

Use this helper for incremental builds and selected test targets. The legacy full-build batch
file also runs gettext and stages the package, which ordinary source edits do not require.

This builds only requested targets, with one MSBuild worker and a bounded number of compilers.

    pwsh -Command "& tools/petkos-dev-build.ps1"           # the DLL and exe, nothing else
    pwsh -Command "& tools/petkos-dev-build.ps1 -Jobs 1"    # one compiler
    pwsh -Command "& tools/petkos-dev-build.ps1 -Full"      # all configured build targets

Two things it deliberately does NOT do:
  - It does not force cmake configuration. CMake regenerates when its inputs change.
  - It does not refresh build/OrcaSlicer/. That staging copy is only needed for a packaged run;
    run-petkos-orca.bat launches build/src/Release/orca-slicer.exe directly, which this updates.
#>
[CmdletBinding()]
param(
    [ValidateRange(1,4)]
    [int]    $Jobs = 2,          # compiler processes, NOT multiplied by MSBuild workers
    [switch] $Full,              # all configured build targets, without package installation
    [switch] $Configure,         # re-run cmake first (only needed when CMakeLists changed)
    [ValidateSet('Release','Debug','RelWithDebInfo','MinSizeRel')]
    [string] $Config = 'Release',
    [ValidateSet('Idle','BelowNormal','Normal')]
    [string] $Priority = 'Idle',
    [string[]] $Targets = @('OrcaSlicer', 'OrcaSlicer_app_gui'),
    [string] $BuildDir = '',
    [string] $LogPath = '',
    [switch] $DryRun
)

$ErrorActionPreference = 'Stop'
$repo  = Split-Path -Parent $PSScriptRoot
$build = if ($BuildDir) { [IO.Path]::GetFullPath($BuildDir) } else { Join-Path $repo 'build' }
if (-not (Test-Path $build)) { throw "No build tree at $build - run build_release_vs2022.bat once first" }
$cache = Join-Path $build 'CMakeCache.txt'
$generatorLine = if (Test-Path $cache) { Select-String -LiteralPath $cache -Pattern '^CMAKE_GENERATOR:INTERNAL=' | Select-Object -First 1 } else { $null }
$generator = if ($generatorLine) { $generatorLine.Line.Split('=', 2)[1] } else { '' }
$isNinja = $generator -eq 'Ninja' -or $generator -eq 'Ninja Multi-Config'
if (-not $isNinja -and -not $generator.StartsWith('Visual Studio ')) {
    throw 'This helper requires a configured Visual Studio or Ninja build tree.'
}
$outputDir = Join-Path $build "src\$Config"
if ($generator -eq 'Ninja') {
    $buildType = (Select-String -LiteralPath $cache -Pattern '^CMAKE_BUILD_TYPE:STRING=' | Select-Object -First 1).Line
    if ($buildType -ne "CMAKE_BUILD_TYPE:STRING=$Config") {
        throw "This single-config Ninja tree is not configured for $Config. Use its configured build type."
    }
    $outputDir = Join-Path $build 'src'
}

$freeGiB = (Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1MB
if ($freeGiB -lt 6) { throw "Less than 6 GiB RAM free; build not started. Close a memory-heavy app before compiling." }
$Jobs = [Math]::Min($Jobs, [Math]::Max(1, [int][Math]::Floor(($freeGiB - 4) / 2)))
$targets = if ($Full) { if ($isNinja) { @('all') } else { @('ALL_BUILD') } } else { $Targets }
if ($targets.Count -eq 0 -or ($targets | Where-Object { $_ -notmatch '^[A-Za-z0-9_.+-]+$' })) {
    throw 'Targets must be CMake target names.'
}
$nativeArgs = if ($isNinja) { @("-j$Jobs") } else { @('-m:1', "-p:CL_MPCount=$Jobs", '-nodeReuse:false', '-clp:ErrorsOnly;Summary') }
if ($DryRun) {
    Write-Output "cmake --build `"$build`" --config $Config --target $($targets -join ' ') -- $($nativeArgs -join ' ')"
    Write-Output "Priority: $Priority; free RAM: $([Math]::Round($freeGiB,1)) GiB; compiler limit: $Jobs"
    Write-Output "Application output: $outputDir"
    return
}
$buildLock = New-Object System.Threading.Mutex($false, 'Local\PodslicerBuild')
if (-not $buildLock.WaitOne(0)) { $buildLock.Dispose(); throw 'Another Podslicer build is already running.' }
try {

if (($targets -contains 'OrcaSlicer' -or $targets -contains 'OrcaSlicer_app_gui' -or $Full) -and
    (Get-Process -Name 'orca-slicer' -ErrorAction SilentlyContinue)) {
    # Windows will not overwrite a mapped image, but it will happily rename one within the same
    # volume - the running process keeps its old mapping and the linker writes fresh files.
    # PODSLICER.md documents the trick; this used to print it as an instruction and then throw,
    # which is a script describing work it is able to do. Killing a running slicer to unblock a
    # link risks a project the user has not saved; renaming risks nothing, so just rename.
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    foreach ($name in @('OrcaSlicer.dll', 'orca-slicer.exe', 'orca-slicer.pdb', 'OrcaSlicer.pdb')) {
        $path = Join-Path $outputDir $name
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
    # directory moves aside. Mapped-image backups remain until explicitly cleaned up.
    $pydir = Join-Path $outputDir 'python'
    if (Test-Path $pydir) {
        $pyaside = Join-Path $outputDir "python.inuse-$stamp"
        $buildPrefix = [IO.Path]::GetFullPath($build).TrimEnd('\') + '\'
        foreach ($movePath in @($pydir, $pyaside)) {
            if (-not [IO.Path]::GetFullPath($movePath).StartsWith($buildPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Build runtime move escapes the build directory: $movePath"
            }
        }
        try {
            Move-Item -LiteralPath $pydir -Destination $pyaside -ErrorAction Stop
            Write-Host "  moved aside (app is running): python\" -ForegroundColor DarkYellow
        } catch {
            throw "orca-slicer is running and python\ could not be moved aside: $_"
        }
    }
}

# Keep mapped-image backups. A build must not decide when user data or backups are deleted.

if ($Configure) {
    Write-Host "cmake configure..." -ForegroundColor Cyan
    & cmake $build
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
}

# orca-slicer.exe is NOT part of the OrcaSlicer target - that one builds OrcaSlicer.dll. The exe
# is a 300 KB launcher from the separate OrcaSlicer_app_gui target, so building 'OrcaSlicer' alone
# leaves whatever exe was already on disk. That was invisible until the app was running and the
# exe had been moved aside, at which point the build produced a fresh DLL and no exe to load it.
$target  = $targets -join ', '
Write-Host "building target $target, config $Config, $Jobs of $([Environment]::ProcessorCount) cores, priority $Priority" -ForegroundColor Cyan

$log = if ($LogPath) { [IO.Path]::GetFullPath($LogPath) } else { Join-Path $build ("petkos-dev-build-{0}.log" -f $Config) }
$sw  = [System.Diagnostics.Stopwatch]::StartNew()

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName  = 'cmake'
$psi.Arguments = "--build `"$build`" --config $Config --target $($targets -join ' ') -- $($nativeArgs -join ' ')"
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError  = $true
$psi.EnvironmentVariables['CMAKE_BUILD_PARALLEL_LEVEL'] = '1'

$self = [System.Diagnostics.Process]::GetCurrentProcess()
$originalPriority = $self.PriorityClass
$self.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::$Priority
try { $p = [System.Diagnostics.Process]::Start($psi) }
finally { $self.PriorityClass = $originalPriority }
# Drain both pipes and write the log while the build is running.
$outTask = $p.StandardOutput.ReadLineAsync()
$errTask = $p.StandardError.ReadLineAsync()
$writer = [IO.StreamWriter]::new($log, $false, [Text.UTF8Encoding]::new($false))
$writer.AutoFlush = $true
try {
    while ($null -ne $outTask -or $null -ne $errTask) {
        if ($null -ne $outTask -and $outTask.IsCompleted) {
            $line = $outTask.GetAwaiter().GetResult()
            if ($null -eq $line) { $outTask = $null }
            else { $writer.WriteLine($line); $outTask = $p.StandardOutput.ReadLineAsync() }
        }
        if ($null -ne $errTask -and $errTask.IsCompleted) {
            $line = $errTask.GetAwaiter().GetResult()
            if ($null -eq $line) { $errTask = $null }
            else { $writer.WriteLine($line); $errTask = $p.StandardError.ReadLineAsync() }
        }
        if (($null -eq $outTask -or -not $outTask.IsCompleted) -and
            ($null -eq $errTask -or -not $errTask.IsCompleted)) {
            [Threading.Thread]::Sleep(100)
        }
    }
    $p.WaitForExit()
} finally { $writer.Dispose() }
$sw.Stop()

$errors = Select-String -Path $log -Pattern 'error [A-Z]+[0-9]+' -AllMatches
if ($p.ExitCode -ne 0 -or $errors) {
    Write-Host "FAILED in $([int]$sw.Elapsed.TotalSeconds)s - $log" -ForegroundColor Red
    $errors | Select-Object -First 20 | ForEach-Object { $_.Line }
    exit 1
}

Write-Host ("OK in {0}m {1:d2}s" -f [int]$sw.Elapsed.TotalMinutes, $sw.Elapsed.Seconds) -ForegroundColor Green
$missing = @()
foreach ($name in $(if ($targets -contains 'OrcaSlicer_app_gui' -or $Full) { @('OrcaSlicer.dll', 'orca-slicer.exe') } else { @() })) {
    $artifact = Join-Path $outputDir $name
    if (Test-Path $artifact) {
        Write-Host ("  {0}  {1}" -f (Get-Item $artifact).LastWriteTime.ToString('HH:mm:ss'), $artifact)
    } else {
        $missing += $name
    }
}
# A green build that produced no runnable app is the failure this reports. MSBuild is happy to
# succeed at a target whose output nothing asked for.
if ($missing) { Write-Host ("  MISSING: {0}" -f ($missing -join ', ')) -ForegroundColor Red; exit 1 }
} finally {
    $buildLock.ReleaseMutex()
    $buildLock.Dispose()
}
