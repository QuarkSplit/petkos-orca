@echo off
REM Petko's Orca dev build - ALWAYS launch through this bat, never the bare exe.
REM --datadir isolates it from the main installed Orca's %%APPDATA%%\OrcaSlicer
REM config, so first-run wizards and version migrations can never clobber the
REM production setup again. Keep this isolated datadir for Podslicer daily use too.
REM
REM Arguments are forwarded to the slicer, so you can open a project directly:
REM   run-petkos-orca.bat "E:\3D-Printing\Projects\Helldivers LAS-58 Talon\LAS-58+Talon+Remastered.3mf"
start "" "D:\Dev\petkos-orca\build\src\Release\orca-slicer.exe" --datadir "D:\Dev\petkos-orca\datadir" %*
