@echo off
setlocal
cd /d "%~dp0"
title Sistema de Seguridad - Instalacion

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\install-system.ps1"

if errorlevel 1 (
  echo.
  echo La instalacion no pudo completarse. Revisa el mensaje anterior.
  pause
)

endlocal

