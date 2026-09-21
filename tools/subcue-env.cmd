@echo off
rem Shared PATH and toolchain lookup for SubCue launchers.
rem Usage: call tools\subcue-env.cmd [cuda|release|debug]
set "SUBCUE_ENV_PRESET=%~1"
if "%SUBCUE_ENV_PRESET%"=="" set "SUBCUE_ENV_PRESET=cuda"
if not defined Qt6_ROOT set "Qt6_ROOT=C:\Qt\6.10.3\msvc2022_64"
if not defined SUBCUE_ROOT set "SUBCUE_ROOT=%~dp0.."
for %%I in ("%SUBCUE_ROOT%") do set "SUBCUE_ROOT=%%~fI"

if /I "%SUBCUE_ENV_PRESET%"=="debug" (
    set "SUBCUE_FFMPEG_BIN=%SUBCUE_ROOT%\vcpkg_installed\x64-windows\debug\bin"
    set "SUBCUE_BUILD_DIR=%SUBCUE_ROOT%\out\build\windows-debug"
) else if /I "%SUBCUE_ENV_PRESET%"=="release" (
    set "SUBCUE_FFMPEG_BIN=%SUBCUE_ROOT%\vcpkg_installed\x64-windows\bin"
    set "SUBCUE_BUILD_DIR=%SUBCUE_ROOT%\out\build\windows-release"
) else (
    set "SUBCUE_FFMPEG_BIN=%SUBCUE_ROOT%\vcpkg_installed\x64-windows\bin"
    set "SUBCUE_BUILD_DIR=%SUBCUE_ROOT%\out\build\windows-release-cuda"
)

if not exist "%Qt6_ROOT%\bin\Qt6Core.dll" if not exist "%Qt6_ROOT%\bin\Qt6Cored.dll" (
    echo [SubCue] Qt runtime not found under %Qt6_ROOT%\bin
    echo [SubCue] Set Qt6_ROOT to the Qt 6 MSVC x64 prefix and retry.
    exit /b 1
)

set "SUBCUE_PATH_PREFIX=%Qt6_ROOT%\bin;%SUBCUE_FFMPEG_BIN%"
if /I "%SUBCUE_ENV_PRESET%"=="cuda" (
    if not defined CUDA_PATH (
        echo [SubCue] CUDA_PATH is not set. Install the CUDA Toolkit and retry.
        exit /b 1
    )
    set "SUBCUE_PATH_PREFIX=%CUDA_PATH%\bin\x64;%CUDA_PATH%\bin;%SUBCUE_PATH_PREFIX%"
)

if /I "%SUBCUE_ENV_PRESET%"=="debug" (
    set "SUBCUE_DEBUG_CRT=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Redist\MSVC\14.51.36231\debug_nonredist\x64\Microsoft.VC145.DebugCRT"
    if not exist "%SUBCUE_DEBUG_CRT%\vcruntime140d.dll" (
        set "SUBCUE_DEBUG_CRT="
        for /d %%D in ("C:\Program Files\Microsoft Visual Studio\18\Community\VC\Redist\MSVC\*") do (
            if exist "%%~fD\debug_nonredist\x64\Microsoft.VC145.DebugCRT\vcruntime140d.dll" set "SUBCUE_DEBUG_CRT=%%~fD\debug_nonredist\x64\Microsoft.VC145.DebugCRT"
        )
    )
    if not defined SUBCUE_DEBUG_CRT (
        echo [SubCue] No x64 Debug CRT found under VC\Redist\MSVC
        echo [SubCue] Start the app from Visual Studio or Qt Creator instead.
        exit /b 1
    )
    set "SUBCUE_SDK_UCRT=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\ucrt"
    if not exist "%SUBCUE_SDK_UCRT%\ucrtbased.dll" (
        set "SUBCUE_SDK_UCRT="
        for /d %%D in ("C:\Program Files (x86)\Windows Kits\10\bin\10.*") do (
            if exist "%%~fD\x64\ucrt\ucrtbased.dll" set "SUBCUE_SDK_UCRT=%%~fD\x64\ucrt"
        )
    )
    if not defined SUBCUE_SDK_UCRT (
        echo [SubCue] No x64 ucrtbased.dll found under the Windows SDK
        echo [SubCue] Start the app from Visual Studio or Qt Creator instead.
        exit /b 1
    )
    set "SUBCUE_PATH_PREFIX=%SUBCUE_PATH_PREFIX%;%SUBCUE_DEBUG_CRT%;%SUBCUE_SDK_UCRT%"
)

set "PATH=%SUBCUE_PATH_PREFIX%;%PATH%"
if not defined SUBCUE_MODELS_ROOT if exist "E:\AIModels\ASR\models" set "SUBCUE_MODELS_ROOT=E:\AIModels\ASR\models"
exit /b 0
