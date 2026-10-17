@echo off
cd /d "%~dp0"
set NESRECOMP_NO_LAUNCHER=1
start "" "%~dp0SuperMarioBros3.exe" "%~dp0Super Mario Bros. 3 (USA).nes"
