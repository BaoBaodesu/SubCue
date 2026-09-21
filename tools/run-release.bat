@echo off
rem Launch the Release CPU build of SubCue. Prefer SubCue.bat / CUDA for daily use.
rem Build first with: tools\build-windows.bat release
setlocal
set "SUBCUE_ROOT=%~dp0.."
call "%~dp0subcue-env.cmd" release
if errorlevel 1 exit /b 1
set "SUBCUE_EXE=%SUBCUE_BUILD_DIR%\SubCue.exe"
if not exist "%SUBCUE_EXE%" (
    echo [SubCue] Missing %SUBCUE_EXE%
    echo [SubCue] Build it first: tools\build-windows.bat release
    exit /b 1
)
"%SUBCUE_EXE%" %*
exit /b %errorlevel%
