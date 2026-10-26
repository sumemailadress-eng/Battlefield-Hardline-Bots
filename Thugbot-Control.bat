@echo off
:: Opens Thugbot Control: start / stop the Hardline bot server, bots per team, pick and switch maps, map rotation.
start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0Thugbot-Control.ps1"
