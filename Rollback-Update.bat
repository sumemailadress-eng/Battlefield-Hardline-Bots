@echo off
:: Undoes the last launcher UPDATE: copies the files it replaced (kept in the newest update_backup_<date> folder) back.
:: Stop the Hardline servers first. Your settings and learned routes were never touched by the update.
setlocal
cd /d "%~dp0"
set LAST=
for /f "delims=" %%d in ('dir /b /ad /o-n update_backup_* 2^>nul') do if not defined LAST set LAST=%%d
if not defined LAST ( echo No update_backup_ folder here - nothing to roll back. & pause & exit /b 1 )
echo Putting back the files from %LAST% ...
xcopy "%LAST%\*" "%~dp0" /e /y /q
if exist "%LAST%\VERSION.txt" ( echo VERSION.txt restored ) else ( del /q VERSION.txt 2>nul )
echo Done. Close and reopen the launcher.
pause
