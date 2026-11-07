@echo off
:: Thugbot Launcher: starts the backend (MySQL, web server, redirector, Hardline emulator), runs Hardline servers 1 - 4
:: with bots and their own map lists / modes, and joins the selected server. (Thugbot-Control.bat is the old panel.)
powershell -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0Thugbot-Launcher.ps1" %*
if errorlevel 1 pause
