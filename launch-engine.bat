@echo off
setlocal
pushd "%~dp0"

set "IRIDIUM_EXE=out\build\x64-release\bin\IridiumEngine.exe"
if not exist "%IRIDIUM_EXE%" set "IRIDIUM_EXE=out\build\x64-debug\bin\IridiumEngine.exe"

if not exist "%IRIDIUM_EXE%" (
    echo Iridium Engine has not been built yet.
    echo Build the x64-release or x64-debug CMake preset, then run this launcher again.
    popd
    pause
    exit /b 1
)

"%IRIDIUM_EXE%" %*
set "IRIDIUM_EXIT_CODE=%ERRORLEVEL%"
popd

if not "%IRIDIUM_EXIT_CODE%"=="0" (
    echo Iridium Engine exited with code %IRIDIUM_EXIT_CODE%.
    pause
)
exit /b %IRIDIUM_EXIT_CODE%
