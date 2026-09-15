@echo off
rem Launch the Debug build of SubCue without entering the full MSVC developer
rem environment.
rem Usage: run-debug.bat [arguments passed to SubCue]
rem Build first with: tools\build-windows.bat
rem
rem Unlike the Release CRT, the Debug CRT (vcruntime140d.dll, ucrtbased.dll)
rem is not on PATH -- not even inside a VS developer command prompt -- so it
rem has to be located explicitly here. For interactive debugging prefer
rem Visual Studio or Qt Creator, which set this up on their own.
setlocal

if not defined Qt6_ROOT set "Qt6_ROOT=C:\Qt\6.10.3\msvc2022_64"
set "SUBCUE_ROOT=%~dp0"
set "SUBCUE_EXE=%SUBCUE_ROOT%\out\build\windows-debug\SubCue.exe"

if not exist "%SUBCUE_EXE%" (
    echo [SubCue] Missing %SUBCUE_EXE%
    echo [SubCue] Build it first: tools\build-windows.bat
    exit /b 1
)

if not exist "%Qt6_ROOT%\bin\Qt6Core.dll" (
    echo [SubCue] Qt runtime not found under %Qt6_ROOT%\bin
    echo [SubCue] Set Qt6_ROOT to the Qt 6 MSVC x64 prefix and retry.
    exit /b 1
)

rem The debug CRT must match the toolset the project builds with; fall back to
rem the newest x64 DebugCRT under VC\Redist when the pinned version is absent.
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

rem ucrtbased.dll ships with the Windows SDK.
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

set "PATH=%Qt6_ROOT%\bin;%SUBCUE_ROOT%\vcpkg_installed\x64-windows\debug\bin;%SUBCUE_DEBUG_CRT%;%SUBCUE_SDK_UCRT%;%PATH%"
"%SUBCUE_EXE%" %*
exit /b %errorlevel%
