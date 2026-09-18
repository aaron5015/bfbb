@echo off
rem Sets FXC to a shader compiler that can emit vs_2_0 and ps_2_0.
rem
rem The June 2010 DirectX SDK is what these scripts were written against and it
rem is still preferred, but it has not shipped for over a decade and a machine
rem with only Visual Studio does not have it. The Windows 10 SDK's fxc compiles
rem the same profiles, so fall back to the newest one installed.
rem
rem Called by the make_*.cmd beside this file. Sets FXC in the caller's
rem environment, so the caller must not have run setlocal first.

set "FXC=%DXSDK_DIR%\utilities\bin\x86\fxc.exe"
if exist "%FXC%" exit /b 0

set "FXC="
for /f "delims=" %%D in ('dir /b /ad /o-n "%ProgramFiles(x86)%\Windows Kits\10\bin\10.*" 2^>nul') do (
	if not defined FXC if exist "%ProgramFiles(x86)%\Windows Kits\10\bin\%%D\x64\fxc.exe" (
		set "FXC=%ProgramFiles(x86)%\Windows Kits\10\bin\%%D\x64\fxc.exe"
	)
)
if defined FXC exit /b 0

echo findfxc: no fxc.exe found. Install the Windows SDK, or set DXSDK_DIR.
exit /b 1
