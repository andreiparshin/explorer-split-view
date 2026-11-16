@echo off
"%~dp0bin\ExplorerSplitController.exe" --stop >nul 2>&1
timeout /t 1 /nobreak >nul
start "" "%~dp0bin\ExplorerSplitController.exe"
