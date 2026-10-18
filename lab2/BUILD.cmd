@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools.ps1" -Action Build
pause
