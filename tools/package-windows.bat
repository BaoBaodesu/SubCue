@echo off
rem Assemble a portable Windows x64 folder with Qt, FFmpeg and VC Runtime.
rem Usage: tools\package-windows.bat [debug|release|cuda]   (default: release)
rem   cuda  packages the CUDA-enabled Release build together with the CUDA runtime DLLs
rem Does not set environment variables named CL, LIB or LINK.
setlocal

set "BUILD_PRESET=windows-release"
set "DIST_DIR=%~dp0..\dist\windows-x64"
if /I "%1"=="debug" (
    set "BUILD_PRESET=windows-debug"
    set "DIST_DIR=%~dp0..\dist\windows-x64-debug"
)
if /I "%1"=="cuda" (
    set "BUILD_PRESET=windows-release-cuda"
    set "DIST_DIR=%~dp0..\dist\windows-x64-cuda"
)

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 exit /b 1

set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "Qt6_ROOT=C:\Qt\6.10.3\msvc2022_64"
set "CXX_COMPILER=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\cl.exe"
set "MSVC_LINKER=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\link.exe"
set "MSVC_AR=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\lib.exe"

if not exist "%~dp0..\out\build\%BUILD_PRESET%\CMakeCache.txt" (
    cmake --fresh --preset %BUILD_PRESET% -DCMAKE_CXX_COMPILER="%CXX_COMPILER%" -DCMAKE_LINKER="%MSVC_LINKER%" -DCMAKE_AR="%MSVC_AR%"
    if errorlevel 1 exit /b 1
)

cmake --build --preset %BUILD_PRESET% --target subcue-windows-package
if errorlevel 1 exit /b 1

set "PACKAGE_DIR=%~dp0..\out\build\%BUILD_PRESET%\package"
if not exist "%PACKAGE_DIR%\SubCue.exe" (
    echo Package folder missing SubCue.exe: %PACKAGE_DIR%
    exit /b 1
)

if exist "%DIST_DIR%" rmdir /s /q "%DIST_DIR%"
mkdir "%DIST_DIR%" 2>nul
xcopy /e /i /y "%PACKAGE_DIR%" "%DIST_DIR%" >nul
if errorlevel 1 exit /b 1
type "%DIST_DIR%\package-size.txt"

rem dist 已保存最终包，清掉构建目录中的重复暂存副本。
rmdir /s /q "%PACKAGE_DIR%"

echo Packed to %DIST_DIR%
endlocal
