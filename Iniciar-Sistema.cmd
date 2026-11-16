@echo off
setlocal
cd /d "%~dp0"
title Sistema de Seguridad - Iniciador

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\start-system.ps1"

if errorlevel 1 (
  echo.
  echo El sistema no pudo iniciarse. Revisa el mensaje anterior.
  pause
)

endlocal

