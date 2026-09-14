<#
Podslicer: syntax-check ONE source file in seconds, without a 30-minute build.

A build is the only honest proof a change compiles, but waiting for one before you can find out
you forgot a semicolon is the reason people ship code they never compiled. This runs the real
compiler, with the real flags that this build tree uses for that exact file, in syntax-only mode
(/Zs: parse and type-check, emit nothing). It catches everything a compile catches except the
link.

    pwsh -File tools/pod-syntax.ps1 src/slic3r/GUI/Plater.cpp
    pwsh -File tools/pod-syntax.ps1 src/slic3r/GUI/Plater.cpp src/libslic3r/PresetBundle.cpp

Flags come from the generated .vcxproj that actually lists the file, so they cannot drift from
what the build does. Exit code 0 means it compiles.
#>
[CmdletBinding()]
param([Parameter(Mandatory, Position=0, ValueFromRemainingArguments)] [string[]] $Files,
      [string] $Config = 'Release')

$ErrorActionPreference = 'Stop'
$repo  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'build'
if (-not (Test-Path $build)) { throw "No build tree at $build" }

# The whole build tree, not just src: tests live in build/tests/... and a test is exactly the
# kind of file worth checking before a build.
$projects = Get-ChildItem -Path $build -Recurse -Filter '*.vcxproj' -ErrorAction SilentlyContinue

function Get-ProjectFor([string] $srcFull) {
    $leaf = Split-Path $srcFull -Leaf
    foreach ($p in $projects) {
        if ($p.Name -like 'encoding-check*') { continue }
        $xml = [xml](Get-Content -LiteralPath $p.FullName -Raw)
        foreach ($c in $xml.Project.ItemGroup.ClCompile) {
            if ($null -eq $c) { continue }
            if ($c.Include -and (Split-Path $c.Include -Leaf) -eq $leaf) {
                if ([IO.Path]::GetFullPath($c.Include) -ieq $srcFull) { return @{ proj = $p.FullName; xml = $xml; item = $c } }
            }
        }
    }
    return $null
}

# The forced includes for ONE source file. CMake puts the precompiled header in per-file
# <ForcedIncludeFiles> rather than in the project-wide block, and files that lean on the PCH for
# their Windows and wxWidgets setup do not compile without it - src/slic3r/GUI/Gizmos is the whole
# directory that does. Missing this made the checker fail every gizmo with an error
# ("'HDITEM': base class undefined") that is a symptom of the missing PCH and looks like a bug in
# the file.
function Get-ForcedIncludes($item, [string] $Config) {
    $out = @()
    if ($null -eq $item) { return $out }
    foreach ($node in @($item.ForcedIncludeFiles)) {
        if ($null -eq $node) { continue }
        $text = if ($node -is [string]) { $node } else { $node.'#text' }
        $cond = if ($node -is [string]) { '' } else { $node.Condition }
        if ($cond -and $cond -notmatch [regex]::Escape("$Config|x64")) { continue }
        if (-not $text) { continue }
        foreach ($one in ($text -split ';')) {
            if ($one -and $one -notmatch '^%\(' -and (Test-Path $one)) { $out += $one }
        }
    }
    return $out | Select-Object -Unique
}

function Get-Flags($xml, [string] $Config) {
    $inc = @(); $def = @(); $opt = @(); $std = ''
    foreach ($idg in $xml.Project.ItemDefinitionGroup) {
        $cond = $idg.Condition
        if ($cond -and $cond -notmatch [regex]::Escape("$Config|x64")) { continue }
        $cl = $idg.ClCompile
        if ($null -eq $cl) { continue }
        if ($cl.AdditionalIncludeDirectories) { $inc += $cl.AdditionalIncludeDirectories -split ';' }
        if ($cl.PreprocessorDefinitions)      { $def += $cl.PreprocessorDefinitions -split ';' }
        # NOT split on whitespace: AdditionalOptions holds '/external:I "D:\path with spaces"',
        # and splitting severs the flag from its argument (cl: D8004). A response-file line is
        # parsed with ordinary command-line rules, so the whole string goes down as one line.
        if ($cl.AdditionalOptions)            { $opt += $cl.AdditionalOptions }
        if ($cl.LanguageStandard)             { $std = $cl.LanguageStandard }
    }
    $inc = $inc | Where-Object { $_ -and $_ -notmatch '^%\(' } | Select-Object -Unique
    $def = $def | Where-Object { $_ -and $_ -notmatch '^%\(' } | Select-Object -Unique
    # /Zs is syntax-only, so anything about codegen, PCH or output files is noise and some of it
    # is actively incompatible with it.
    # AdditionalOptions is where CMake puts the bulk of the include path, as
    # /external:I "D:/some/dir" pairs. Rather than trying to keep a flag and its quoted argument
    # together through a token list, lift every such path out and re-add it as a plain /I - the
    # only thing /external: buys here is warning suppression, and /Zs emits no warnings worth
    # having. Everything else in the string is codegen or output-file noise that /Zs rejects.
    $optText = ($opt -join ' ')
    foreach ($m in [regex]::Matches($optText, '/external:I\s+"([^"]+)"|/external:I\s+(\S+)')) {
        $dir = if ($m.Groups[1].Success) { $m.Groups[1].Value } else { $m.Groups[2].Value }
        if ($dir) { $inc += $dir }
    }
    $inc = $inc | Where-Object { $_ } | Select-Object -Unique
    # Keep only the handful of semantic switches; drop the rest rather than allow-listing noise.
    $opt = @([regex]::Matches($optText, '/(Zc:\w+(-)?|permissive-|utf-8|bigobj|std:c\+\+\w+|EHsc|D\S+)') |
             ForEach-Object { '/' + $_.Groups[1].Value })
    $stdFlag = switch ($std) { 'stdcpp17' { '/std:c++17' } 'stdcpp20' { '/std:c++20' } 'stdcpplatest' { '/std:c++latest' } default { '/std:c++17' } }
    return @{ inc = $inc; def = $def; opt = $opt; std = $stdFlag }
}

$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64 not found at $vcvars" }

$failed = @()
foreach ($f in $Files) {
    $srcFull = [IO.Path]::GetFullPath((Join-Path $repo $f))
    if (-not (Test-Path $srcFull)) { $srcFull = [IO.Path]::GetFullPath($f) }
    if (-not (Test-Path $srcFull)) { Write-Host "NOT FOUND: $f" -ForegroundColor Red; $failed += $f; continue }

    $found = Get-ProjectFor $srcFull
    $viaShim = $false
    if ($null -eq $found) {
        # A header is not compiled by anything, so no project lists it - and a header is exactly
        # what is most worth checking, because a mistake in one breaks every translation unit that
        # includes it. Borrow the flags of a sibling .cpp in the same directory (same target, same
        # include path by construction) and compile a one-line file that includes the header. That
        # is what a compiler does to a header anyway.
        $sibling = Get-ChildItem -LiteralPath (Split-Path $srcFull -Parent) -Filter '*.cpp' -ErrorAction SilentlyContinue |
                   ForEach-Object { Get-ProjectFor $_.FullName } | Where-Object { $_ } | Select-Object -First 1
        if ($null -eq $sibling) {
            Write-Host "no vcxproj compiles $f, and no sibling .cpp in its directory is compiled either" -ForegroundColor Yellow
            $failed += $f; continue
        }
        $found = $sibling
        $viaShim = $true
    }
    $flags = Get-Flags $found.xml $Config

    $compileTarget = $srcFull
    $shimPath = $null
    if ($viaShim) {
        # Beside the header, so its own relative includes resolve exactly as they do in real use.
        $shimPath = Join-Path (Split-Path $srcFull -Parent) ("pod-syntax-shim-" + [IO.Path]::GetFileNameWithoutExtension($srcFull) + ".cpp")
        # Twice: a header that is not include-guarded fails here rather than in somebody's build.
        [IO.File]::WriteAllText($shimPath, "#include `"$(Split-Path $srcFull -Leaf)`"`r`n#include `"$(Split-Path $srcFull -Leaf)`"`r`nint pod_syntax_shim() { return 0; }`r`n")
        $compileTarget = $shimPath
    }

    $argFile = [IO.Path]::GetTempFileName()
    # ONLY what this build actually uses. An earlier version added /permissive- and
    # /Zc:__cplusplus of its own accord, which made this checker STRICTER than the compiler that
    # builds the tree: the project sets no ConformanceMode, so MSVC's permissive extensions are
    # on, and binding a temporary to a non-const reference - which this tree does, in code that
    # has compiled for years - was reported as an error. A checker that fails code the build
    # accepts is worse than none, because it sends people to fix things that are not broken.
    $lines = @('/c', '/Zs', '/nologo', '/EHsc', '/bigobj', $flags.std)
    foreach ($fi in (Get-ForcedIncludes $found.item $Config)) { $lines += "/FI`"$fi`"" }
    $lines += $flags.opt
    $lines += ($flags.def | ForEach-Object { "/D$_" })
    $lines += ($flags.inc | ForEach-Object { "/I`"$_`"" })
    $lines += "`"$compileTarget`""
    # cl reads a response file as ANSI/UTF-16 unless told otherwise; ASCII keeps every path literal.
    [IO.File]::WriteAllLines($argFile, $lines, [Text.ASCIIEncoding]::new())

    Write-Host "syntax-check $f  ($(Split-Path $found.proj -Leaf))" -ForegroundColor Cyan
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $out = & cmd /c "call `"$vcvars`" >nul 2>&1 && cl @`"$argFile`" 2>&1"
    $rc = $LASTEXITCODE
    $sw.Stop()
    Remove-Item $argFile -Force -ErrorAction SilentlyContinue
    if ($shimPath) { Remove-Item $shimPath -Force -ErrorAction SilentlyContinue }

    $errors = $out | Where-Object { $_ -match 'error [A-Z]+[0-9]+' }
    if ($rc -ne 0 -or $errors) {
        Write-Host ("  FAILED in {0}s" -f [int]$sw.Elapsed.TotalSeconds) -ForegroundColor Red
        $errors | Select-Object -First 25 | ForEach-Object { Write-Host "  $_" }
        if (-not $errors) { $out | Select-Object -Last 15 | ForEach-Object { Write-Host "  $_" } }
        $failed += $f
    } else {
        Write-Host ("  OK in {0}s" -f [int]$sw.Elapsed.TotalSeconds) -ForegroundColor Green
        $out | Where-Object { $_ -match 'warning C4(715|701|702|456|457)' } | Select-Object -First 5 | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkYellow }
    }
}
if ($failed) { exit 1 }
