@echo off
chcp 65001 >nul
setlocal
set "SCRIPT=%~dp0Install-FufuPlugin.ps1"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%"
set "EXITCODE=%ERRORLEVEL%"
if not "%EXITCODE%"=="0" pause
exit /b %EXITCODE%
