@echo off
rem Builds Cardinal for Windows: the core (CMake + vcpkg), its tests, then the WinUI 3 app.
rem Needs Visual Studio 2026 (or 2022) with "Desktop development with C++" and the WinUI C++
rem tools, and VCPKG_ROOT pointing at a vcpkg checkout.
rem   tools\build-windows.cmd            build everything and run the core tests
rem   tools\build-windows.cmd notest     skip the tests
setlocal
set ROOT=%~dp0..
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (echo Visual Studio with the C++ tools was not found. & exit /b 1)
if not defined VCPKG_ROOT (echo Set VCPKG_ROOT to your vcpkg checkout. & exit /b 1)
set KEEP_VCPKG_ROOT=%VCPKG_ROOT%
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
rem vcvars points VCPKG_ROOT at the copy bundled with Visual Studio; keep the user's.
set VCPKG_ROOT=%KEEP_VCPKG_ROOT%

pushd "%ROOT%"
cmake --preset windows || goto :fail
cmake --build --preset windows || goto :fail
if /i not "%1"=="notest" (ctest --preset windows || goto :fail)
msbuild ui-win\Cardinal.vcxproj -t:restore -p:RestorePackagesConfig=true -nologo -v:minimal || goto :fail
msbuild ui-win\Cardinal.vcxproj -p:Configuration=Debug -p:Platform=x64 -nologo -v:minimal -m || goto :fail
popd
echo.
echo Built build\ui-win\x64\Debug\Cardinal.exe
exit /b 0

:fail
popd
exit /b 1
