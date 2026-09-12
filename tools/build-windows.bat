@echo off
rem SubCue Windows build script: configure, build and run CTest inside the
rem MSVC x64 developer environment.
rem Usage: tools\build-windows.bat [debug|release]   (default: debug)
rem Do NOT set environment variables named CL, LIB or LINK.
setlocal

set "BUILD_PRESET=windows-debug"
if /I "%1"=="release" set "BUILD_PRESET=windows-release"

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 exit /b 1

set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "Qt6_ROOT=C:\Qt\6.10.3\msvc2022_64"
set "CXX_COMPILER=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\cl.exe"
set "C_COMPILER=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\cl.exe"
set "MSVC_LINKER=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\link.exe"
set "MSVC_AR=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\lib.exe"

cmake --fresh --preset %BUILD_PRESET% -DCMAKE_C_COMPILER="%C_COMPILER%" -DCMAKE_CXX_COMPILER="%CXX_COMPILER%" -DCMAKE_LINKER="%MSVC_LINKER%" -DCMAKE_AR="%MSVC_AR%"
if errorlevel 1 exit /b 1
cmake --build --preset %BUILD_PRESET%
if errorlevel 1 exit /b 1
ctest --preset %BUILD_PRESET% --output-on-failure
if errorlevel 1 exit /b 1

endlocal
