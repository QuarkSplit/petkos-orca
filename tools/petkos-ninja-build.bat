@echo off
REM Podslicer: build the Ninja + ccache tree.
REM
REM Ninja is used instead of the Visual Studio generator for two measured reasons:
REM   * a four-file change cost 38m19s under MSBuild, of which the link was 42 SECONDS and the
REM     rest was 270 translation units recompiled because a hub header changed;
REM   * adding two source files to a CMakeLists cost a full 81-minute rebuild, because
REM     regenerating the .vcxproj files invalidates every object. Ninja's graph survives a
REM     reconfigure.
REM Ninja also runs one cl per file across all targets at once, which is what replaces /MP -
REM and /MP has to go, because ccache cannot cache a compile that emits many objects.
REM
REM Must run inside vcvars64: Ninja takes whatever compiler is on PATH, and Strawberry Perl's
REM MinGW g++ is on this machine's PATH.
setlocal
set VS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || (echo vcvars64 failed & exit /b 1)

set BUILD=D:\Dev\petkos-orca\build-ninja
if not exist "%BUILD%\build.ninja" (
  echo no build.ninja - run tools\petkos-ninja-configure.bat first
  exit /b 1
)
cd /d "%BUILD%"

REM The exe is a separate target from the DLL; building only OrcaSlicer leaves a stale launcher.
set TARGETS=OrcaSlicer OrcaSlicer_app_gui
if "%~1" neq "" set TARGETS=%*

echo == ninja %TARGETS%
ccache -z >nul 2>&1
cmake --build . --target %TARGETS%
set RC=%ERRORLEVEL%
echo == ccache for this build:
ccache -s | findstr /I "Hits Misses Cacheable Uncacheable"
exit /b %RC%
