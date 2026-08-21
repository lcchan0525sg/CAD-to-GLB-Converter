@echo off
setlocal
cd /d "%~dp0"
set "PATH=%~dp0;%PATH%"
start "CAD Converter 2" "%~dp0cad-converter2.exe"
endlocal
