@echo off
setlocal
cd /d "%~dp0"
if exist "..\VulkanSDK\1.4.321.1\Bin\glslc.exe" (
    set "VULKAN_SDK=%~dp0..\VulkanSDK\1.4.321.1"
    set "VK_ADD_LAYER_PATH=%~dp0..\VulkanSDK\1.4.321.1\Bin"
)
if not exist "build-debug\Debug\vulkan-starter-app.exe" (
    echo Application not found. Run BUILD.cmd first.
    pause
    exit /b 1
)
"build-debug\Debug\vulkan-starter-app.exe" > "build-debug\runtime-output.log" 2> "build-debug\runtime-errors.log"
if errorlevel 1 pause
