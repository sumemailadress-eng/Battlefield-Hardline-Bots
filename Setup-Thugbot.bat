@echo off
:: Thugbot setup: finds the BF-Portable backend, Hardline server, Blaze emulator, BF Launcher and game (or lets you point
:: at them), saves them in Thugbot-Paths.ini and installs the bots into the server.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Setup-Thugbot.ps1"
if errorlevel 1 pause
