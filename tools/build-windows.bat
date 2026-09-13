@echo off
rem SubCue Windows build script: configure, build and run CTest inside the
rem MSVC x64 developer environment.
rem Usage: tools\build-windows.bat [debug|release|cuda] [fresh]   (default: release)
rem   debug|release|cuda  select Debug, Release CPU, or Release CUDA
rem   fresh          wipe and reconfigure the build directory before building;
rem                  only needed after toolchain changes or a broken cache
rem Do NOT set environment variables named CL, LIB or LINK.
setlocal

set "BUILD_PRESET=windows-release"
if /I "%1"=="debug" set "BUILD_PRESET=windows-debug"
if /I "%1"=="cuda" set "BUILD_PRESET=windows-release-cuda"
set "SUBCUE_FRESH="
if /I "%1"=="fresh" set "SUBCUE_FRESH=1"
if /I "%2"=="fresh" set "SUBCUE_FRESH=1"

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 exit /b 1

set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "Qt6_ROOT=C:\Qt\6.10.3\msvc2022_64"
set "CXX_COMPILER=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\cl.exe"
set "C_COMPILER=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\cl.exe"
set "MSVC_LINKER=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\link.exe"
set "MSVC_AR=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\lib.exe"

set "BUILD_DIR=%~dp0..\out\build\%BUILD_PRESET%"
if defined SUBCUE_FRESH if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"

if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo [SubCue] Configuring %BUILD_PRESET%
    cmake --fresh --preset %BUILD_PRESET% -DCMAKE_C_COMPILER="%C_COMPILER%" -DCMAKE_CXX_COMPILER="%CXX_COMPILER%" -DCMAKE_LINKER="%MSVC_LINKER%" -DCMAKE_AR="%MSVC_AR%"
    if errorlevel 1 exit /b 1
) else (
    echo [SubCue] Reusing %BUILD_DIR% - pass "fresh" to reconfigure
)

cmake --build --preset %BUILD_PRESET%
if errorlevel 1 exit /b 1
ctest --preset %BUILD_PRESET% --output-on-failure
if errorlevel 1 exit /b 1

endlocal
