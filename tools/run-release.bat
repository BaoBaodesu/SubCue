@echo off
rem Launch the Release build of SubCue without entering the full MSVC
rem developer environment.
rem Usage: tools\run-release.bat [arguments passed to SubCue]
rem Build first with: tools\build-windows.bat release
rem
rem The Release CRT lives in System32, so only the Qt and FFmpeg runtime
rem directories have to be prepended here.
setlocal

if not defined Qt6_ROOT set "Qt6_ROOT=C:\Qt\6.10.3\msvc2022_64"
set "SUBCUE_ROOT=%~dp0.."
set "SUBCUE_EXE=%SUBCUE_ROOT%\out\build\windows-release\SubCue.exe"

if not exist "%SUBCUE_EXE%" (
    echo [SubCue] Missing %SUBCUE_EXE%
    echo [SubCue] Build it first: tools\build-windows.bat release
    exit /b 1
)

if not exist "%Qt6_ROOT%\bin\Qt6Core.dll" (
    echo [SubCue] Qt runtime not found under %Qt6_ROOT%\bin
    echo [SubCue] Set Qt6_ROOT to the Qt 6 MSVC x64 prefix and retry.
    exit /b 1
)

set "PATH=%Qt6_ROOT%\bin;%SUBCUE_ROOT%\vcpkg_installed\x64-windows\bin;%PATH%"
"%SUBCUE_EXE%" %*
exit /b %errorlevel%
