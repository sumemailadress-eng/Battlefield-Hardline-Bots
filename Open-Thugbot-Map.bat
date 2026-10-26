@echo off
:: Opens the learned-data map of the map being played (or the last one): saved routes, spawns, flags, stuck spots,
:: doors / glass, where bots die and where the bots are. The server rewrites it every minute; reload the page to update.
:: Each map also keeps its own page in the same folder (Thugbot_report\<map>_<mode>.html). The server folder comes from
:: Thugbot-Paths.ini (Setup-Thugbot.bat).
set SERVER=
for /f "usebackq tokens=1,* delims==" %%a in ("%~dp0Thugbot-Paths.ini") do if /i "%%a"=="Server" set SERVER=%%b
if "%SERVER%"=="" (
    echo The server folder is not set. Run Setup-Thugbot.bat first.
    pause
    exit /b 1
)
set REPORT=%SERVER%\Thugbot_report\latest.html
if not exist "%REPORT%" (
    echo No map page yet. Start the server with Start-Thugbot.bat and give it a minute.
    pause
    exit /b 1
)
start "" "%REPORT%"
