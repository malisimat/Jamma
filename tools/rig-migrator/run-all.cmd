@echo off
setlocal
rem Inventory all rigs, copy compatible sessions, and remap defaults last.
rem Add -DryRun to preview; explicit directory arguments override profile defaults.
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0migrate-rig.ps1" -CopyProfile %*
set "migrationExitCode=%errorlevel%"
if "%~1"=="" pause
exit /b %migrationExitCode%
