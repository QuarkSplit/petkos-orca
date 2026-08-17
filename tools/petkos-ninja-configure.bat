@echo off
REM Podslicer: configure the Ninja + ccache build tree.
REM
REM Must run inside vcvars64. With the Visual Studio generator CMake uses MSVC by definition;
REM with Ninja it takes whatever compiler is on PATH, and on this machine Strawberry Perl's
REM MinGW g++ wins - which makes CMAKE_CXX_COMPILER_ID come out "GNU", OpenCV pick its mingw
REM runtime, and the wxWidgets package config compute an empty platform directory. All three
REM read as separate dependency problems and are one wrong compiler.
setlocal
set VS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || (echo vcvars64 failed & exit /b 1)

set REPO=D:\Dev\petkos-orca
set BUILD=%REPO%\build-ninja
if not exist "%BUILD%" mkdir "%BUILD%"
cd /d "%BUILD%"

cmake "%REPO%" -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DORCA_TOOLS=ON ^
  -DSLIC3R_CCACHE=ON ^
  -DCMAKE_C_COMPILER=cl ^
  -DCMAKE_CXX_COMPILER=cl ^
  -DCMAKE_PREFIX_PATH="%REPO%/deps/build/OrcaSlicer_dep/usr/local" %*
exit /b %ERRORLEVEL%
