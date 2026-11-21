@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%.") do set "SCRIPT_DIR=%%~fI"

set "DIST_ROOT=%SCRIPT_DIR%"
set "OVERLAY_DIR=%DIST_ROOT%\release-overlay"
if not exist "%OVERLAY_DIR%" set "OVERLAY_DIR=%DIST_ROOT%\default-overlay"
set "RAZE_EXE=%DIST_ROOT%\raze.exe"
set "PREFLIGHT_PS1=%DIST_ROOT%\tools\dist\Prepare-CommercialNormals.ps1"
set "LAUNCH_VARS=%DIST_ROOT%\generated-content\state\launch-duke-rt-vars.cmd"

if not exist "%RAZE_EXE%" (
    set "DIST_ROOT=%SCRIPT_DIR%\..\.."
    for %%I in ("%DIST_ROOT%") do set "DIST_ROOT=%%~fI"
    set "OVERLAY_DIR=%DIST_ROOT%\release-overlay"
    if not exist "%OVERLAY_DIR%" set "OVERLAY_DIR=%DIST_ROOT%\default-overlay"
    set "RAZE_EXE=%DIST_ROOT%\build\terminal-release\raze.exe"
    if not exist "%RAZE_EXE%" set "RAZE_EXE=%DIST_ROOT%\build\terminal-ninja\raze.exe"
    set "PREFLIGHT_PS1=%DIST_ROOT%\tools\dist\Prepare-CommercialNormals.ps1"
    set "LAUNCH_VARS=%DIST_ROOT%\generated-content\state\launch-duke-rt-vars.cmd"
)

if not exist "%RAZE_EXE%" (
    echo [duke-rt] raze.exe was not found beside the launcher or at the repo fallback path.
    exit /b 1
)

if not exist "%PREFLIGHT_PS1%" (
    echo [duke-rt] preflight script was not found: "%PREFLIGHT_PS1%"
    exit /b 1
)

set "PREFLIGHT_ARGS="
set "GAME_ARGS="

:parse_args
if "%~1"=="" goto args_done

if /I "%~1"=="-Ask" (
    set PREFLIGHT_ARGS=%PREFLIGHT_ARGS% -Ask
    shift
    goto parse_args
)
if /I "%~1"=="-Yes" (
    set PREFLIGHT_ARGS=%PREFLIGHT_ARGS% -Yes
    shift
    goto parse_args
)
if /I "%~1"=="-No" (
    set PREFLIGHT_ARGS=%PREFLIGHT_ARGS% -No
    shift
    goto parse_args
)
if /I "%~1"=="-Force" (
    set PREFLIGHT_ARGS=%PREFLIGHT_ARGS% -Force
    shift
    goto parse_args
)
if /I "%~1"=="-Quiet" (
    set PREFLIGHT_ARGS=%PREFLIGHT_ARGS% -Quiet
    shift
    goto parse_args
)
if /I "%~1"=="-VoxelAsk" (
    set PREFLIGHT_ARGS=%PREFLIGHT_ARGS% -VoxelAsk
    shift
    goto parse_args
)
if /I "%~1"=="-VoxelYes" (
    set PREFLIGHT_ARGS=%PREFLIGHT_ARGS% -VoxelYes
    shift
    goto parse_args
)
if /I "%~1"=="-VoxelNo" (
    set PREFLIGHT_ARGS=%PREFLIGHT_ARGS% -VoxelNo
    shift
    goto parse_args
)
if /I "%~1"=="-ForceVoxels" (
    set PREFLIGHT_ARGS=%PREFLIGHT_ARGS% -ForceVoxels
    shift
    goto parse_args
)
if /I "%~1"=="-VoxelZip" (
    if "%~2"=="" (
        echo [duke-rt] -VoxelZip requires a path argument.
        exit /b 1
    )
    set PREFLIGHT_ARGS=%PREFLIGHT_ARGS% -VoxelZip "%~2"
    shift
    shift
    goto parse_args
)
if /I "%~1"=="-HrpAsk" (
    set PREFLIGHT_ARGS=%PREFLIGHT_ARGS% -HrpAsk
    shift
    goto parse_args
)
if /I "%~1"=="-HrpYes" (
    set PREFLIGHT_ARGS=%PREFLIGHT_ARGS% -HrpYes
    shift
    goto parse_args
)
if /I "%~1"=="-HrpNo" (
    set PREFLIGHT_ARGS=%PREFLIGHT_ARGS% -HrpNo
    shift
    goto parse_args
)
if /I "%~1"=="-HrpZip" (
    if "%~2"=="" (
        echo [duke-rt] -HrpZip requires a path argument.
        exit /b 1
    )
    set PREFLIGHT_ARGS=%PREFLIGHT_ARGS% -HrpZip "%~2"
    shift
    shift
    goto parse_args
)
rem A user -file folder with its own duke3d.def (a DEF pack) shadows the overlay's.
if /I "%~1"=="-file" (
    if not "%~2"=="" (
        if exist "%~f2\duke3d.def" set "USER_DEF_PACK=1"
        set GAME_ARGS=%GAME_ARGS% -file "%~2"
        shift
        shift
        goto parse_args
    )
)
if /I "%~1"=="-SourceRoot" (
    if "%~2"=="" (
        echo [duke-rt] -SourceRoot requires a path argument.
        exit /b 1
    )
    set PREFLIGHT_ARGS=%PREFLIGHT_ARGS% -SourceRoot "%~2"
    shift
    shift
    goto parse_args
)
if /I "%~1"=="-GameRoot" (
    if "%~2"=="" (
        echo [duke-rt] -GameRoot requires a path argument.
        exit /b 1
    )
    set PREFLIGHT_ARGS=%PREFLIGHT_ARGS% -GameRoot "%~2"
    shift
    shift
    goto parse_args
)
if /I "%~1"=="-OverlayDir" (
    if "%~2"=="" (
        echo [duke-rt] -OverlayDir requires a path argument.
        exit /b 1
    )
    set "OVERLAY_DIR=%~f2"
    shift
    shift
    goto parse_args
)

set GAME_ARGS=%GAME_ARGS% %1
shift
goto parse_args

:args_done

if not exist "%OVERLAY_DIR%" (
    echo [duke-rt] overlay directory was not found: "%OVERLAY_DIR%"
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%PREFLIGHT_PS1%" -LaunchRoot "%DIST_ROOT%" -OverlayDir "%OVERLAY_DIR%" -LaunchVarsPath "%LAUNCH_VARS%" %PREFLIGHT_ARGS%
if errorlevel 1 (
    echo [duke-rt] content preflight failed; a valid DUKE3D.GRP path is required.
    exit /b 1
)

if not exist "%LAUNCH_VARS%" (
    echo [duke-rt] preflight did not write launch state: "%LAUNCH_VARS%"
    exit /b 1
)

call "%LAUNCH_VARS%"

if not exist "%DUKE_RT_GRP%" (
    echo [duke-rt] resolved DUKE3D.GRP was not found: "%DUKE_RT_GRP%"
    exit /b 1
)

rem The installed High Resolution Pack is mounted after the overlay. Raze loads only
rem one duke3d.def (the last mounted), so when a DEF pack (the HRP, or a user -file
rem folder with its own duke3d.def) shadows the overlay's, re-add the DEFs that file
rem includes (Duke-RT maphacks and the voxel pack) with -adddef. Without a DEF pack
rem the overlay's duke3d.def already includes them; adding them again would load
rem every voxel twice.
set "CONTENT_ARGS="
set "DEF_PACK="
if defined USER_DEF_PACK set "DEF_PACK=1"
if defined DUKE_RT_HRP_DIR if exist "%DUKE_RT_HRP_DIR%\duke3d.def" (
    set CONTENT_ARGS=-file "%DUKE_RT_HRP_DIR%"
    set "DEF_PACK=1"
)
if defined DEF_PACK if exist "%OVERLAY_DIR%\duke3d_maphacks.def" set CONTENT_ARGS=%CONTENT_ARGS% -adddef duke3d_maphacks.def
if defined DEF_PACK if exist "%OVERLAY_DIR%\duke3d_voxels.def" set CONTENT_ARGS=%CONTENT_ARGS% -adddef duke3d_voxels.def

"%RAZE_EXE%" -gamegrp "%DUKE_RT_GRP%" -file "%OVERLAY_DIR%" %CONTENT_ARGS% %GAME_ARGS%
exit /b %errorlevel%
