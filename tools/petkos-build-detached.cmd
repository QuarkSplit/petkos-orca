@echo off
REM Scheduler entry point. All build paths share the bounded, low-priority policy.
setlocal
set "LOG=%TEMP%\petkos-build-detached.log"
set "DONE=%TEMP%\petkos-build-detached.done"
pwsh -NoProfile -File "%~dp0petkos-dev-build.ps1" -LogPath "%LOG%" > "%LOG%.driver" 2>&1
set "RC=%ERRORLEVEL%"
echo %RC% > "%DONE%"
endlocal
