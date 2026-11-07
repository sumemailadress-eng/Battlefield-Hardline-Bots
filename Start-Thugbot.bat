@echo off
:: Starts BF-Portable's backend, installs Thugbot and launches the Hardline server with bots. Arguments go to
:: Start-Thugbot.ps1 (e.g. Start-Thugbot.bat -Bots 24).
:: Use the launcher: with no arguments this just opens it.
if "%~1"=="" if exist "%~dp0Thugbot-Launcher.bat" (start "" "%~dp0Thugbot-Launcher.bat" & exit /b 0)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-Thugbot.ps1" %*
if errorlevel 1 pause
