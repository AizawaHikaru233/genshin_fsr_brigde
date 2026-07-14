@echo off
setlocal
chcp 65001>nul
set "ROOT=%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%Installer.ps1"
if errorlevel 1 pause
endlocal
