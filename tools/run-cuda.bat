@echo off
rem Launch the CUDA-enabled Release build of SubCue.
rem Build first with: tools\build-windows.bat cuda
setlocal

if not defined Qt6_ROOT set "Qt6_ROOT=C:\Qt\6.10.3\msvc2022_64"
if not defined CUDA_PATH (
    echo [SubCue] CUDA_PATH is not set. Install the CUDA Toolkit and retry.
    exit /b 1
)
set "SUBCUE_ROOT=%~dp0.."
set "SUBCUE_EXE=%SUBCUE_ROOT%\out\build\windows-release-cuda\SubCue.exe"
if not exist "%SUBCUE_EXE%" set "SUBCUE_EXE=%SUBCUE_ROOT%\out\build\verify-cuda-nmake\SubCue.exe"

if not exist "%SUBCUE_EXE%" (
    echo [SubCue] Missing %SUBCUE_EXE%
    echo [SubCue] Build it first: tools\build-windows.bat cuda
    exit /b 1
)

set "PATH=%CUDA_PATH%\bin\x64;%CUDA_PATH%\bin;%Qt6_ROOT%\bin;%SUBCUE_ROOT%\vcpkg_installed\x64-windows\bin;%PATH%"
"%SUBCUE_EXE%" %*
exit /b %errorlevel%
