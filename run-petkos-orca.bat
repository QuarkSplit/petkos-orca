@echo off
REM Petko's Orca dev build - ALWAYS launch through this bat, never the bare exe.
REM --datadir isolates it from the main installed Orca's %%APPDATA%%\OrcaSlicer
REM config, so first-run wizards and version migrations can never clobber the
REM production setup again. When petkos-orca is promoted to daily driver,
REM remove the --datadir arg (and uninstall the Program Files Orca).
REM
REM Arguments are forwarded to the slicer, so you can open a project directly:
REM   run-petkos-orca.bat "E:\3D-Printing\Projects\3D WiPs\LAS-58+Talon+Remastered.3mf"
start "" "D:\Dev\petkos-orca\build\src\Release\orca-slicer.exe" --datadir "D:\Dev\petkos-orca\datadir" %*
