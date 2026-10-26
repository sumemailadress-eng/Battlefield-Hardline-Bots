@echo off
:: Route hotkeys (F5 record, F6 save, F7 point, F8 undo, F9 clear, F10 elevator/ladder, F11 bots out). Start-Thugbot.bat
:: starts this by itself; run it by hand if the server was started another way. Another player name: Thugbot-Keys.bat -Player Name
start "" /min powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0Thugbot-Keys.ps1" %*
