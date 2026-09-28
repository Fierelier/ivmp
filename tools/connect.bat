@echo off
setlocal

if "%~1"=="" goto usage
if "%~2"=="" goto usage

set "ivmp_dir=%~1"
set "ivmp_server=%~2"
set "ivmp_nickname=%~3"
if "%ivmp_nickname%"=="" set "ivmp_nickname=Player"

copy /y "%~dp0..\bin\xlive.dll" "%ivmp_dir%\xlive.dll" >nul
if errorlevel 1 (
	echo could not copy xlive.dll into "%ivmp_dir%"
	exit /b 1
)

setx IVMP_SERVER "%ivmp_server%" >nul
setx IVMP_NICKNAME "%ivmp_nickname%" >nul
setx IVMP_REDIRECT_ALL 1 >nul

echo xlive.dll installed in "%ivmp_dir%"
echo connection set to %ivmp_server% as %ivmp_nickname%
echo log out and back in, or reboot, then launch GTAIV.exe directly
goto :eof

:usage
echo usage: connect.bat ^<gtaiv folder^> ^<host:port^> [nickname]
exit /b 2
