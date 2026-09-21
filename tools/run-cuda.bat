@echo off
rem Launch the CUDA-enabled Release build of SubCue.
rem Build first with: tools\build-windows.bat
setlocal
set "SUBCUE_ROOT=%~dp0.."
call "%~dp0subcue-env.cmd" cuda
if errorlevel 1 exit /b 1
set "SUBCUE_EXE=%SUBCUE_BUILD_DIR%\SubCue.exe"
if not exist "%SUBCUE_EXE%" set "SUBCUE_EXE=%SUBCUE_ROOT%\out\build\windows-release-cuda\SubCue.exe"
if not exist "%SUBCUE_EXE%" (
    echo [SubCue] Missing %SUBCUE_EXE%
    echo [SubCue] Build it first: tools\build-windows.bat
    exit /b 1
)
"%SUBCUE_EXE%" %*
exit /b %errorlevel%
