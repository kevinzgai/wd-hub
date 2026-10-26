@echo off
REM WorkBuddy 看板服务启动器（Windows 双击即可）
REM 注意：必须用 py 启动。本机 python 指向 WorkBuddy 自带的 3.13，没装 fastapi。
chcp 65001 >nul
cd /d "%~dp0.."

py -c "import fastapi, uvicorn, psutil" 2>nul
if errorlevel 1 (
    echo [错误] 依赖缺失，请先执行：
    echo     py -m pip install -r server\requirements.txt
    pause
    exit /b 1
)

echo 正在启动看板服务...
py -m server.api_server --port 8080
pause
