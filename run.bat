@echo off
setlocal
cd /d "%~dp0"
set "PATH=%~dp0;%PATH%"
start "CAD to GLB Convertor" "%~dp0cad-converter2.exe"
endlocal
