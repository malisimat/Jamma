@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0migrate-rig.ps1" %*
set "migrationExitCode=%errorlevel%"
if "%~1"=="" pause
exit /b %migrationExitCode%
