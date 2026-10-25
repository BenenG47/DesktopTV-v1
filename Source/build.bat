@echo off
rem Builds DesktopTV.addon64 with the Visual Studio C++ tools. Double-click it.
setlocal
pushd "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="
if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
if not exist "%VCVARS%" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo Could not find vcvars64.bat - tell Claude.
  goto :end
)
echo Using "%VCVARS%"
call "%VCVARS%" >nul
if not exist build mkdir build
cl /nologo /LD /EHsc /O2 /MT /std:c++17 /W3 /DNOMINMAX /I reshade_include /Fo"build\\" src\desktoptv_addon.cpp /link d3d11.lib dxgi.lib user32.lib ole32.lib /OUT:build\DesktopTV.addon64 > build\build_log.txt 2>&1
type build\build_log.txt
if exist build\DesktopTV.addon64 (
  echo.
  echo BUILD OK: build\DesktopTV.addon64
) else (
  echo.
  echo BUILD FAILED - the log is in build\build_log.txt
)
:end
popd
echo.
pause
