@echo off
REM Petko's Orca: run a build that nothing in an agent session can kill.
REM
REM A full rebuild here is 30-35 minutes. A build launched from an agent's shell is a child of that
REM shell, and when the shell is stopped the top-level cmake dies with it - MSBuild's worker nodes
REM carry on compiling for a while, so the tree fills with fresh .obj files and then simply stops,
REM having never linked. That looks exactly like a finished build until you check the timestamp on
REM the DLL. It cost two cycles.
REM
REM Run this through Task Scheduler instead (see petkos-build-detached.ps1). The task is owned by
REM the scheduler service, not by whatever launched it, so it runs to completion or fails honestly.
REM
REM Writes:  %TEMP%\petkos-build-detached.log   the full build output
REM          %TEMP%\petkos-build-detached.done  the exit code, written last, as the completion signal

setlocal
set REPO=%~dp0..
set LOG=%TEMP%\petkos-build-detached.log
set DONE=%TEMP%\petkos-build-detached.done

if exist "%DONE%" del /q "%DONE%"

REM Windows will not overwrite a mapped image but will happily rename one, so a running slicer is
REM moved aside rather than killed - it may be holding a project nobody has saved.
for %%F in (OrcaSlicer.dll orca-slicer.exe OrcaSlicer.pdb orca-slicer.pdb) do (
    if exist "%REPO%\build\src\Release\%%F" (
        ren "%REPO%\build\src\Release\%%F" "%%~nF.inuse-%RANDOM%%%~xF" 2>nul
    )
)

echo === petkos build started %DATE% %TIME% > "%LOG%"
cmake --build "%REPO%\build" --config Release --target OrcaSlicer OrcaSlicer_app_gui -- -m:14 -clp:ErrorsOnly;Summary >> "%LOG%" 2>&1
set RC=%ERRORLEVEL%
echo === petkos build finished %DATE% %TIME% rc=%RC% >> "%LOG%"

REM Written last and only once, so a poller that sees this file knows the build is genuinely over.
echo %RC% > "%DONE%"
endlocal
