@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..\..") do set "REPO_ROOT=%%~fI"

set "VSDEVCMD=C:\PROGRA~1\MICROS~1\2022\COMMUN~1\Common7\Tools\VsDevCmd.bat"
if defined RAZE_VSDEVCMD set "VSDEVCMD=%RAZE_VSDEVCMD%"

if not exist "%VSDEVCMD%" (
    echo [release-package] VsDevCmd was not found: "%VSDEVCMD%"
    echo [release-package] Set RAZE_VSDEVCMD to your local VsDevCmd.bat path if needed.
    exit /b 1
)

set "VCPKG_OVERLAY_PORTS=%REPO_ROOT%\vcpkg-overlays"
set "VCPKG_CMAKE_CONFIGURE_OPTIONS=-DCMAKE_POLICY_DEFAULT_CMP0026=OLD"
set "TOOLCHAIN_FILE=%REPO_ROOT%\build\vcpkg\scripts\buildsystems\vcpkg.cmake"
set "VCPKG_INSTALLED_DIR=%REPO_ROOT%\vcpkg_installed"
set "ZMUSIC_SOURCE=%REPO_ROOT%\build\zmusic"
set "ZMUSIC_BUILD=%REPO_ROOT%\build\zmusic\build-ninja-ovl2"
set "RAZE_BUILD=%REPO_ROOT%\build\terminal-release"
set "ZMUSIC_INCLUDE_DIR=%REPO_ROOT%\build\zmusic\include"
set "ZMUSIC_LIBRARIES=%REPO_ROOT%\build\zmusic\build-ninja-ovl2\source\zmusiclite.lib"

call "%VSDEVCMD%" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%

cmake -G Ninja -S "%ZMUSIC_SOURCE%" -B "%ZMUSIC_BUILD%" -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="%TOOLCHAIN_FILE%" -DVCPKG_LIBSNDFILE=1 -DVCPKG_INSTALLED_DIR="%VCPKG_INSTALLED_DIR%" -DVCPKG_OVERLAY_PORTS="%VCPKG_OVERLAY_PORTS%"
if errorlevel 1 exit /b %errorlevel%

cmake --build "%ZMUSIC_BUILD%" --target zmusiclite
if errorlevel 1 exit /b %errorlevel%

cmake -G Ninja -S "%REPO_ROOT%" -B "%RAZE_BUILD%" -DCMAKE_BUILD_TYPE=Release -DRAZE_NRI_SHADER_PROFILE=PRODUCTION -DCMAKE_TOOLCHAIN_FILE="%TOOLCHAIN_FILE%" -DVCPKG_INSTALLED_DIR="%VCPKG_INSTALLED_DIR%" -DVCPKG_OVERLAY_PORTS="%VCPKG_OVERLAY_PORTS%" -DZMUSIC_INCLUDE_DIR="%ZMUSIC_INCLUDE_DIR%" -DZMUSIC_LIBRARIES="%ZMUSIC_LIBRARIES%"
if errorlevel 1 exit /b %errorlevel%

cmake --build "%RAZE_BUILD%" --target raze
if errorlevel 1 exit /b %errorlevel%

powershell -ExecutionPolicy Bypass -File "%SCRIPT_DIR%Build-WindowsReleasePackage.ps1" -SkipBuild %*
exit /b %errorlevel%
