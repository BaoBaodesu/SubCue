@echo off
rem Launch SubCue from the project root. Rebuilds keep working because the
rem executable is resolved at launch time from the current CMake preset.
rem Usage: SubCue.bat [arguments passed to SubCue]
rem Override the build with: set SUBCUE_PRESET=windows-release
setlocal
set "SUBCUE_ROOT=%~dp0"
if "%SUBCUE_ROOT:~-1%"=="\" set "SUBCUE_ROOT=%SUBCUE_ROOT:~0,-1%"

set "SUBCUE_ENV=cuda"
if /I "%SUBCUE_PRESET%"=="windows-debug" set "SUBCUE_ENV=debug"
if /I "%SUBCUE_PRESET%"=="windows-release" set "SUBCUE_ENV=release"
if /I "%SUBCUE_PRESET%"=="windows-release-cuda" set "SUBCUE_ENV=cuda"

call "%SUBCUE_ROOT%\tools\subcue-env.cmd" %SUBCUE_ENV%
if errorlevel 1 exit /b 1

if defined SUBCUE_PRESET (
    set "SUBCUE_EXE=%SUBCUE_ROOT%\out\build\%SUBCUE_PRESET%\SubCue.exe"
) else (
    set "SUBCUE_EXE=%SUBCUE_ROOT%\out\build\windows-release-cuda\SubCue.exe"
)

if not exist "%SUBCUE_EXE%" (
    echo [SubCue] Missing %SUBCUE_EXE%
    echo [SubCue] Build it first: tools\build-windows.bat
    exit /b 1
)

"%SUBCUE_EXE%" %*
exit /b %errorlevel%
