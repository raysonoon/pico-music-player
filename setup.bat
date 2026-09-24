@echo off
rem Pico Music Player - one-time setup + bridge launcher
cd /d "%~dp0"

where python >nul 2>nul
if errorlevel 1 (
    echo.
    echo Python was not found.
    echo Install Python 3.12 from https://www.python.org/downloads/
    echo and tick "Add python.exe to PATH" during installation.
    echo.
    pause
    exit /b 1
)

python setup.py
pause