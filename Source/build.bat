@echo off
:: Builds the Thugbot loader (proxy xinput9_1_0.dll) and the hot-reloadable core (ThugbotCore.dll), 64-bit, with the
:: Visual Studio 2019 Build Tools (x64 cl.exe).
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist out mkdir out
cl /nologo /O2 /W3 /MT /LD /Foout\ src\loader.c /Fe:out\xinput9_1_0.dll /link kernel32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /W3 /MT /LD /D_CRT_SECURE_NO_WARNINGS /Foout\ src\core.c src\nav.c src\learn.c src\trace_nav.c /Fe:out\ThugbotCore.dll /link kernel32.lib
if errorlevel 1 exit /b 1
echo Built out\xinput9_1_0.dll and out\ThugbotCore.dll
