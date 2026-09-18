@echo off
rem Sets DXC to a shader compiler that can emit SPIR-V.
rem
rem The dxc.exe in the Windows SDK cannot: it is built without SPIR-V codegen and
rem says so when asked for -spirv. The Vulkan SDK's can, and so can the release
rem builds at https://github.com/microsoft/DirectXShaderCompiler/releases.
rem
rem Set DXC to one of those to pick it outright. Otherwise the Vulkan SDK's is
rem used when VULKAN_SDK is set.
rem
rem Called by the make_*.cmd beside this file. Sets DXC in the caller's
rem environment, so the caller must not have run setlocal first.

if defined DXC if exist "%DXC%" exit /b 0

set "DXC=%VULKAN_SDK%\Bin\dxc.exe"
if defined VULKAN_SDK if exist "%DXC%" exit /b 0

set "DXC="
echo finddxc: no dxc.exe with SPIR-V output found. Set DXC to one, or VULKAN_SDK.
exit /b 1
