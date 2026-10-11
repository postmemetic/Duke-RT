@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-WindowsReleasePackage.ps1" %*
exit /b %errorlevel%
