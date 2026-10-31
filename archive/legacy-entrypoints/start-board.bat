@echo off
setlocal
title WorkBuddy Board Service
cd /d "%~dp0"

echo ============================================================
echo   WorkBuddy Board Service
echo   Close this window to stop the service.
echo ============================================================
echo.

where py >nul 2>nul
if errorlevel 1 goto :no_py

py -c "import fastapi, uvicorn, psutil" >nul 2>nul
if errorlevel 1 goto :install_deps
goto :check_port

:install_deps
echo [INFO] Dependencies missing, installing now (about 1 min)...
echo.
py -m pip install -r server\requirements.txt
if errorlevel 1 goto :dep_failed
echo.
goto :check_port

:check_port
netstat -ano | findstr ":27777" | findstr "LISTENING" >nul 2>nul
if not errorlevel 1 goto :port_busy

echo Starting... the device will connect within 5 seconds.
echo.
py -m server.api_server --port 27777
set "RC=%errorlevel%"
goto :done

:no_py
echo [ERROR] Python launcher "py" not found.
echo.
echo   Install Python 3 and tick "Add python.exe to PATH":
echo   https://www.python.org/downloads/
echo.
goto :hold

:dep_failed
echo.
echo [ERROR] Failed to install dependencies. Run this manually:
echo.
echo   py -m pip install -r server\requirements.txt
echo.
goto :hold

:port_busy
echo [INFO] Port 27777 is already in use - the service may already be running.
echo.
echo   If the device screen shows ONLINE, just close this window.
echo   To restart, close the old black window first, then double-click again.
echo.
echo   Process holding 27777:
netstat -ano | findstr ":27777" | findstr "LISTENING"
echo.
goto :hold

:done
echo.
echo ------------------------------------------------------------
if "%RC%"=="0" (
    echo Service stopped normally.
) else (
    echo [WARN] Service exited with code %RC%
    echo        Send the messages above to the developer.
)
echo ------------------------------------------------------------

:hold
echo Press any key to close...
pause >nul
endlocal
