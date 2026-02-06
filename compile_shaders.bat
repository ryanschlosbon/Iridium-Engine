@echo off
:: Instead of a hardcoded path, we use the %VULKAN_SDK% environment variable
set GLSLC="%VULKAN_SDK%/Bin/glslc.exe"

if not exist %GLSLC% (
    echo ERROR: Vulkan SDK not found. Please ensure it is installed and VULKAN_SDK is set.
    pause
    exit /b
)

cd assets/shaders

for %%f in (*.vert) do (
    echo Compiling %%f...
    %GLSLC% %%f -o %%~nf_vert.spv
)

for %%f in (*.frag) do (
    echo Compiling %%f...
    %GLSLC% %%f -o %%~nf_frag.spv
)

echo.
echo --------------------------------------
echo Shaders Compiled Successfully!
echo --------------------------------------
pause