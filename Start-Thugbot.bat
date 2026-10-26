@echo off
:: Starts BF-Portable's backend, installs Thugbot and launches the Hardline server with bots. Arguments go to
:: Start-Thugbot.ps1 (e.g. Start-Thugbot.bat -Bots 24).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-Thugbot.ps1" %*
if errorlevel 1 pause
