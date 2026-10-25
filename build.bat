@echo off
rem Builds the 32-bit xNVSE plugin out\rowe.dll.
rem Needs Visual Studio 2017 or newer (or its Build Tools) with the C++ x86/x64 tools and a
rem Windows SDK (for the D3D9 headers and fxc). Run it from any folder.
setlocal

rem Use an existing x86 developer environment, otherwise find Visual Studio with vswhere.
if /i "%VSCMD_ARG_TGT_ARCH%"=="x86" goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo vswhere.exe not found: install Visual Studio or the VS Build Tools & exit /b 1)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo No Visual Studio installation with the C++ tools found & exit /b 1)
rem (vcvars32 prints harmless "not found" messages on some installs, hence 2>nul.)
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul 2>nul
where cl >nul 2>nul || (echo vcvars32.bat did not set up the compiler & exit /b 1)

:build
set "SRC=%~dp0src"
set "OUT=%~dp0out"
if not exist "%OUT%" mkdir "%OUT%"

rem Shaders are compiled to headers (byte arrays) and built into the DLL.
fxc /nologo /T vs_3_0 /E VSMain /Vn g_lensVS /Fh "%OUT%\lens_vs.h" "%SRC%\lens.hlsl" >nul || exit /b 1
fxc /nologo /T ps_3_0 /E PSMain /Vn g_lensPS /Fh "%OUT%\lens_ps.h" "%SRC%\lens.hlsl" >nul || exit /b 1
fxc /nologo /T ps_3_0 /E PSCursor /Vn g_cursorPS /Fh "%OUT%\cursor_ps.h" "%SRC%\lens.hlsl" >nul || exit /b 1
fxc /nologo /T vs_3_0 /E VSMain /Vn g_downscaleVS /Fh "%OUT%\downscale_vs.h" "%SRC%\downscale.hlsl" >nul || exit /b 1
fxc /nologo /T ps_3_0 /E PSMain /Vn g_downscalePS /Fh "%OUT%\downscale_ps.h" "%SRC%\downscale.hlsl" >nul || exit /b 1
fxc /nologo /T ps_3_0 /E PSFast /Vn g_downscaleFastPS /Fh "%OUT%\downscale_fast_ps.h" "%SRC%\downscale.hlsl" >nul || exit /b 1

rem /MT links the C runtime statically, so the DLL needs no Visual C++ redistributable.
rem /d1trimfile: source paths compiled in (Dear ImGui's asserts) are relative to this folder, so no
rem local folder (user name, build directory) ends up in the binaries.
set "ROOT=%~dp0"
set "TRIM=/d1trimfile:%ROOT:~0,-1%"
rem Version information, and for the two programs an icon and a manifest (res\rowe.rc).
for %%r in (rowe_dll rowe_launcher rowe_mo2setup) do (
  rc /nologo /i"%~dp0res" /fo"%OUT%\%%r.res" "%~dp0res\%%r.rc" || exit /b 1
)
cl /nologo /O2 /MT /EHsc /W3 /std:c++17 /D_CRT_SECURE_NO_WARNINGS "%TRIM%" /LD /I"%OUT%" /I"%~dp0third_party\reshade" /I"%~dp0third_party\imgui" /I"%~dp0third_party\fonts" /DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD /Fo"%OUT%\\" /Fe"%OUT%\rowe.dll" ^
  "%SRC%\main.cpp" "%SRC%\detour.cpp" "%SRC%\crashlog.cpp" "%SRC%\settings.cpp" "%SRC%\keynames.cpp" "%SRC%\oldnames.cpp" "%SRC%\png.cpp" ^
  "%SRC%\d3d9hook.cpp" "%SRC%\frame.cpp" "%SRC%\screenshot.cpp" "%SRC%\overlay.cpp" ^
  "%SRC%\fov.cpp" "%SRC%\lens.cpp" "%SRC%\mousetrap.cpp" "%SRC%\intros.cpp" "%SRC%\supersample.cpp" "%SRC%\dinput.cpp" "%SRC%\ui.cpp" ^
  "%SRC%\recon_win32.cpp" "%SRC%\recon_engine.cpp" "%SRC%\gamecmd.cpp" "%SRC%\reshade_addon.cpp" "%SRC%\restart.cpp" "%SRC%\gameui.cpp" "%SRC%\gamefont.cpp" "%SRC%\hudstyle.cpp" "%SRC%\borderless.cpp" "%SRC%\mcm.cpp" "%SRC%\autosave.cpp" "%SRC%\photo.cpp" "%SRC%\mo2setup.cpp" "%SRC%\mo2common.cpp" "%OUT%\rowe_dll.res" ^
  "%~dp0third_party\imgui\imgui.cpp" "%~dp0third_party\imgui\imgui_draw.cpp" "%~dp0third_party\imgui\imgui_tables.cpp" "%~dp0third_party\imgui\imgui_widgets.cpp" ^
  "%~dp0third_party\imgui\backends\imgui_impl_dx9.cpp" "%~dp0third_party\imgui\backends\imgui_impl_win32.cpp" ^
  /link user32.lib kernel32.lib advapi32.lib dxguid.lib ole32.lib oleaut32.lib windowscodecs.lib shell32.lib || exit /b 1
echo Built %OUT%\rowe.dll

rem The launcher (rowe_launcher.exe): save picker and game settings, same look as the F8 menu.
if not exist "%OUT%\launcher" mkdir "%OUT%\launcher"
cl /nologo /O2 /MT /EHsc /W3 /std:c++17 /D_CRT_SECURE_NO_WARNINGS "%TRIM%" /I"%SRC%" /I"%~dp0third_party\imgui" /I"%~dp0third_party\fonts" /DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD /D"ImDrawIdx=unsigned int" /Fo"%OUT%\launcher\\" /Fe"%OUT%\rowe_launcher.exe" ^
  "%SRC%\launcher\launcher.cpp" "%SRC%\launcher\gamefiles.cpp" "%OUT%\rowe_launcher.res" "%SRC%\keynames.cpp" "%SRC%\oldnames.cpp" "%SRC%\hudstyle.cpp" "%SRC%\gamefont.cpp" ^
  "%~dp0third_party\imgui\imgui.cpp" "%~dp0third_party\imgui\imgui_draw.cpp" "%~dp0third_party\imgui\imgui_tables.cpp" "%~dp0third_party\imgui\imgui_widgets.cpp" ^
  "%~dp0third_party\imgui\backends\imgui_impl_dx9.cpp" "%~dp0third_party\imgui\backends\imgui_impl_win32.cpp" ^
  /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib kernel32.lib advapi32.lib shell32.lib ole32.lib d3d9.lib dwmapi.lib || exit /b 1
echo Built %OUT%\rowe_launcher.exe

rem The Mod Organizer 2 setup helper (rowe_mo2setup.exe), installed only with the installer's MO2 options.
if not exist "%OUT%\mo2setup" mkdir "%OUT%\mo2setup"
cl /nologo /O2 /MT /EHsc /W3 /std:c++17 /D_CRT_SECURE_NO_WARNINGS "%TRIM%" /I"%SRC%" /Fo"%OUT%\mo2setup\\" /Fe"%OUT%\rowe_mo2setup.exe" ^
  "%SRC%\mo2helper.cpp" "%SRC%\mo2common.cpp" "%OUT%\rowe_mo2setup.res" ^
  /link /SUBSYSTEM:WINDOWS user32.lib kernel32.lib advapi32.lib shell32.lib ole32.lib || exit /b 1
echo Built %OUT%\rowe_mo2setup.exe

rem The optional plugin that lists ROWE in The Mod Configuration Menu (tools\make_mcm_esp.py; needs Python).
python "%~dp0tools\make_mcm_esp.py" "%OUT%\ROWE - MCM.esp" || echo Python not found: "ROWE - MCM.esp" not written
