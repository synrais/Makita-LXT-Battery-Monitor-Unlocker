@echo off
title Makita Battery Monitor — Build APK
echo.
echo ============================================
echo   Makita Battery Monitor — APK Builder
echo ============================================
echo.

:: Convert current Windows path to WSL path and run the build script
wsl bash -c "cd \"$(wslpath '%cd%')\" && chmod +x build_Makita_Battery_Monitor_apk.sh && ./build_Makita_Battery_Monitor_apk.sh"

if errorlevel 1 (
    echo.
    echo ERROR: Build failed. See output above.
    pause
    exit /b 1
)

pause
