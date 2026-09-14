@echo off
rem The screen passes' shader blobs, for both Direct3D backends and Vulkan, from
rem the sources beside this file.
rem
rem The same arrangement as librw's third_party/librw/src/d3d/shaders: each
rem shader is compiled at ps_2_0 into this directory for D3D9, at ps_4_0 with
rem SM4 defined into ..\shaders11 for D3D11, and to SPIR-V from the SM4 source
rem into ..\shadersvk for Vulkan, under one array name. librw's rwshader.h is
rem the whole of what differs between the first two. The blobs are checked in so
rem the build needs no shader compiler. Run this when a .hlsl changes, and
rem commit every tree's .h with it.
set "LIBRWSHADERS=%~dp0..\..\..\..\..\..\third_party\librw\src\d3d\shaders"
call "%LIBRWSHADERS%\findfxc.cmd" || exit /b 1
call "%LIBRWSHADERS%\finddxc.cmd" || exit /b 1
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

rem call :ps NAME SOURCE -- ps_2_0 here, ps_4_0 in ..\shaders11, SPIR-V in ..\shadersvk
rem
rem The SPIR-V flags are librw's; its make_shaders.cmd says what the bindings are.
:ps
"%FXC%" /nologo /T ps_2_0 /I "%LIBRWSHADERS%" /Vn %1 /Fh %1.h %2 || exit /b 1
"%FXC%" /nologo /T ps_4_0 /DSM4 /I "%LIBRWSHADERS%" /Vn %1 /Fh ..\shaders11\%1.h %2 || exit /b 1
"%DXC%" -nologo -T ps_6_0 -E main -spirv -fspv-target-env=vulkan1.1 -fspv-reflect -fvk-bind-globals 2 0 -fvk-t-shift 3 0 -fvk-s-shift 7 0 -DSM4 -I "%LIBRWSHADERS%" -Fo "%TEMP%\bfbbvk_%1.spv" %2 || exit /b 1
python "%LIBRWSHADERS%\spirv_h.py" "%TEMP%\bfbbvk_%1.spv" %1 ..\shadersvk\%1.h || exit /b 1
del "%TEMP%\bfbbvk_%1.spv"
exit /b 0
