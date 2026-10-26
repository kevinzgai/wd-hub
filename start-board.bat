@echo off
setlocal
title WorkBuddy 看板服务
cd /d "%~dp0"

echo ============================================================
echo   WorkBuddy 看板服务
echo   关闭本窗口即停止服务
echo ============================================================
echo.

REM ---------- 1. 找 py 启动器 ----------
where py >nul 2>nul
if errorlevel 1 goto :no_py

REM ---------- 2. 查依赖 ----------
py -c "import fastapi, uvicorn, psutil" >nul 2>nul
if errorlevel 1 goto :install_deps
goto :check_port

:install_deps
echo [提示] 缺少依赖，正在自动安装（首次约 1 分钟）...
echo.
py -m pip install -r server\requirements.txt
if errorlevel 1 goto :dep_failed
echo.
goto :check_port

REM ---------- 3. 查端口占用 ----------
:check_port
netstat -ano | findstr ":8080" | findstr "LISTENING" >nul 2>nul
if not errorlevel 1 goto :port_busy

REM ---------- 4. 启动 ----------
echo 正在启动，设备会在 5 秒内自动连上...
echo.
py -m server.api_server --port 8080
set "RC=%errorlevel%"
goto :done

:no_py
echo [错误] 找不到 py 启动器。
echo.
echo   请安装 Python 3，安装时勾选 "Add python.exe to PATH"：
echo   https://www.python.org/downloads/
echo.
goto :hold

:dep_failed
echo.
echo [错误] 依赖安装失败。请手动执行下面这条命令后重试：
echo.
echo   py -m pip install -r server\requirements.txt
echo.
goto :hold

:port_busy
echo [提示] 8080 端口已被占用，服务可能已经在运行了。
echo.
echo   若设备屏幕显示"在线"，说明无需再启动，直接关掉本窗口即可。
echo   若确实要重启，请先关掉原来那个黑窗口，再双击本文件。
echo.
echo   当前占用 8080 的进程：
netstat -ano | findstr ":8080" | findstr "LISTENING"
echo.
goto :hold

:done
echo.
echo ------------------------------------------------------------
if "%RC%"=="0" (
    echo 服务已正常退出。
) else (
    echo [警告] 服务异常退出，退出码 %RC%
    echo        请把上面的错误信息发给开发者。
)
echo ------------------------------------------------------------

:hold
echo 按任意键关闭窗口...
pause >nul
endlocal
