@echo off
rem The screen passes' shader blobs, for both Direct3D backends, from the
rem sources beside this file.
rem
rem The same arrangement as librw's third_party/librw/src/d3d/shaders: each
rem shader is compiled at ps_2_0 into this directory for D3D9 and at ps_4_0 with
rem SM4 defined into ..\shaders11 for D3D11, under one array name, and librw's
rem rwshader.h is the whole of what differs between the two. The blobs are
rem checked in so the build needs no shader compiler. Run this when a .hlsl
rem changes, and commit both trees' .h with it.
set "LIBRWSHADERS=%~dp0..\..\..\..\..\..\third_party\librw\src\d3d\shaders"
call "%LIBRWSHADERS%\findfxc.cmd" || exit /b 1
pushd "%~dp0" || exit /b 1

call :ps distort_PS distort_PS.hlsl || goto fail
call :ps glow_bright_PS glow_bright_PS.hlsl || goto fail
call :ps glow_blur_PS glow_blur_PS.hlsl || goto fail

popd
echo All shaders compiled.
exit /b 0

:fail
popd
exit /b 1

rem call :ps NAME SOURCE -- ps_2_0 here, ps_4_0 in ..\shaders11
:ps
"%FXC%" /nologo /T ps_2_0 /I "%LIBRWSHADERS%" /Vn %1 /Fh %1.h %2 || exit /b 1
"%FXC%" /nologo /T ps_4_0 /DSM4 /I "%LIBRWSHADERS%" /Vn %1 /Fh ..\shaders11\%1.h %2 || exit /b 1
exit /b 0
