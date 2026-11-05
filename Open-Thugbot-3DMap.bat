@echo off
:: Thugbot 3D map: the bots' raycast scan of a map as a 3D point cloud, with the routes they load drawn on top.
:: Draw, move and delete routes in the page and save them back into the map's route files (bots read them at map load).
:: Needs Node.js. Leave the black "Thugbot 3D map server" window open while you use the page; close it when done.
:: The server folder comes from Thugbot-Paths.ini (Setup-Thugbot.bat).
cd /d "%~dp0tools\Map3D"
where node >nul 2>nul || (echo Node.js is not installed - get it from https://nodejs.org & pause & exit /b 1)
start "Thugbot 3D map server" /min node server.js
timeout /t 2 /nobreak >nul
start "" http://localhost:8643/
