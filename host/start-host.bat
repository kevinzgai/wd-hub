@echo off
rem ===========================================================================
rem  WorkBuddy host launcher (Windows)
rem
rem  Pure ASCII on purpose: cmd.exe reads .bat with the OEM codepage (936 on
rem  zh-CN), so UTF-8 Chinese inside a .bat gets cut mid-character and breaks
rem  line parsing. wb_app.py prints Chinese logs in this window already.
rem
rem  Default action = RESTART: stop whatever Python is holding the port, then
rem  run the host in this window (close the window or Ctrl+C to stop).
rem
rem    start-host.bat                    Restart on 0.0.0.0:8080
rem    start-host.bat --no-restart        Start only if nothing is running
rem    start-host.bat --stop              Stop the running instance
rem    start-host.bat --status            Show running state
rem    start-host.bat --help              This help
rem
rem  Change port:  set WEB_PORT=9090   then run the same command (--stop too).
rem ===========================================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "PORT=8080"
if not "%WEB_PORT%"=="" set "PORT=%WEB_PORT%"
set "PY=%~dp0.venv\Scripts\python.exe"
set "APP=%~dp0wb_app.py"
set "ACTION=restart"

if /i "%~1"=="--help" goto HELP
if /i "%~1"=="-h" goto HELP
if /i "%~1"=="--stop" goto STOP
if /i "%~1"=="--status" goto STATUS
if /i "%~1"=="--no-restart" goto NORESTART
if not "%~1"=="" (
  echo [ERROR] Unknown option: %~1
  goto HELP
)
goto BEGIN

:NORESTART
set "ACTION=start"
goto BEGIN

:BEGIN
if not exist "%PY%" (
  echo [ERROR] Missing .venv\Scripts\python.exe
  echo         Create once:  py -3 -m venv .venv
  echo         Then:         .venv\Scripts\python.exe -m pip install bleak pystray Pillow certifi
  goto HOLD
)
rem Guard against the classic "system python has no bleak" trap: Web UI would
rem still start, but the device would never be found.
"%PY%" -c "import bleak" >nul 2>&1
if errorlevel 1 (
  echo [ERROR] bleak is missing in .venv - BLE scan would silently fail.
  echo         Run: "%PY%" -m pip install bleak pystray Pillow certifi
  goto HOLD
)
if not exist "%APP%" (
  echo [ERROR] wb_app.py not found next to this script.
  goto HOLD
)

call :FINDPID
if defined OLD (
  if /i "!ACTION!"=="start" (
    echo [INFO] Host already running ^(PID !OLD!^), http://127.0.0.1:%PORT%
    goto HOLD
  )
  set "IMG="
  for /f "tokens=1 delims=," %%n in ('tasklist /FI "PID eq !OLD!" /FO CSV /NH 2^>nul') do set "IMG=%%~n"
  echo !IMG! | findstr /i "python" >nul
  if errorlevel 1 (
    echo [WARN] Port %PORT% is held by !IMG! ^(PID !OLD!^), not Python. Refusing to kill it.
    goto HOLD
  )
  echo Stopping previous host ^(PID !OLD!^) ...
  taskkill /PID !OLD! /T /F >nul 2>&1
  ping -n 2 127.0.0.1 >nul
)

echo Starting WorkBuddy host: http://127.0.0.1:%PORT%  ^(LAN 0.0.0.0^)
echo Close this window or press Ctrl+C to stop.
echo.
title WorkBuddy host - port %PORT%
"%PY%" -u "%APP%" --web-host 0.0.0.0 --web-port %PORT%
set "RC=%errorlevel%"
echo.
echo [INFO] Host exited with code %RC%.
goto HOLD

:STOP
call :FINDPID
if not defined OLD (
  echo [INFO] No running host on port %PORT%.
  goto END
)
echo Stopping host ^(PID !OLD!^) ...
taskkill /PID !OLD! /T /F >nul 2>&1
echo [INFO] Stopped.
goto END

:STATUS
call :FINDPID
if defined OLD (
  echo [INFO] Running: PID !OLD!  http://127.0.0.1:%PORT%
) else (
  echo [INFO] Not running on port %PORT%.
)
goto END

:HELP
echo.
echo  WorkBuddy host launcher ^(Windows^)
echo.
echo    start-host.bat                  Restart host on 0.0.0.0:8080 ^(stops old instance first^)
echo    start-host.bat --no-restart      Start only if nothing is running
echo    start-host.bat --stop            Stop the running instance
echo    start-host.bat --status          Show running state
echo    start-host.bat --help            This help
echo.
echo  Change port:  set WEB_PORT=9090   then run the same command ^(also for --stop^)
echo  Logs stay in this window; close it to stop the host.
echo.
goto END

:FINDPID
set "OLD="
for /f "tokens=5" %%a in ('netstat -ano ^| findstr ":%PORT% " ^| findstr /i "LISTENING"') do set "OLD=%%a"
goto :eof

:HOLD
echo.
echo Press any key to close this window...
pause >nul

:END
endlocal
exit /b 0
