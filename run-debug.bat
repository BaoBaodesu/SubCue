@echo off
rem Launch the Debug build of SubCue. Prefer SubCue.bat / CUDA for daily use.
rem Build first with: tools\build-windows.bat debug
setlocal
set "SUBCUE_ROOT=%~dp0"
call "%SUBCUE_ROOT%tools\subcue-env.cmd" debug
if errorlevel 1 exit /b 1
set "SUBCUE_EXE=%SUBCUE_BUILD_DIR%\SubCue.exe"
if not exist "%SUBCUE_EXE%" (
    echo [SubCue] Missing %SUBCUE_EXE%
    echo [SubCue] Build it first: tools\build-windows.bat debug
    exit /b 1
)
"%SUBCUE_EXE%" %*
exit /b %errorlevel%
