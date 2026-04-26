@echo off
title Makita Battery Monitor — Build EXE
echo.
echo ============================================
echo   Makita Battery Monitor — EXE Builder
echo ============================================
echo.

:: Check Python is available
python --version >nul 2>&1
if errorlevel 1 (
    echo ERROR: Python not found. Install from https://python.org
    pause
    exit /b 1
)

echo [1/3] Installing dependencies...
pip install pyserial pyinstaller --quiet
if errorlevel 1 (
    echo ERROR: pip install failed.
    pause
    exit /b 1
)

echo.
echo [2/3] Building Makita_Battery_Monitor.exe...
pyinstaller ^
    --onefile ^
    --console ^
    --name "Makita_Battery_Monitor" ^
    --hidden-import serial ^
    --hidden-import serial.tools ^
    --hidden-import serial.tools.list_ports ^
    makita_battery_monitor.py

if errorlevel 1 (
    echo.
    echo ERROR: Build failed. See output above.
    pause
    exit /b 1
)

echo.
echo [3/3] Cleaning up build files...
rmdir /s /q build
del /q Makita_Monitor.spec

echo.
echo ============================================
echo   Done.  dist\Makita_Battery_Monitor.exe is ready.
echo ============================================
echo.
pause
