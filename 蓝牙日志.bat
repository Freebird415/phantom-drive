@echo off
chcp 936 >nul
title 工创小车 - 蓝牙日志实时接收
cd /d "%~dp0"

rem ============================================================
rem  蓝牙日志实时接收（图形界面）
rem  用途：车不用插线，跑车/推车/转轮子时无线把日志收到电脑，
rem        窗口里能实时看到每一帧（灰度/误差/PWM/编码器增量）。
rem  只需常识：车上总开关打开；电脑蓝牙已开。
rem  数据：logs\<时间>_<备注>\telemetry.csv（与 btlog.py 同一格式）
rem  GBK 编码，中文 Windows 双击即用，不需要敲任何命令
rem ============================================================

where python >nul 2>&1
if errorlevel 1 (
    echo [错误] 没有检测到 Python。
    echo 请先安装：https://www.python.org/downloads/  （安装时勾选 Add to PATH）
    echo.
    pause
    exit /b 1
)

python -c "import bleak" >nul 2>&1
if errorlevel 1 (
    echo [准备] 首次使用，正在安装蓝牙库 bleak ...
    python -m pip install bleak
    if errorlevel 1 (
        echo.
        echo [错误] 安装失败，请把本窗口截图发到群里。
        echo.
        pause
        exit /b 1
    )
)

echo 正在打开蓝牙日志窗口……
python tools\btview.py
if errorlevel 1 (
    echo.
    echo [错误] 程序异常退出。常见原因：
    echo   1. 车上总开关没开（模块不在广播，界面里会提示"没找到设备"）
    echo   2. 电脑蓝牙被关闭
    echo.
    pause
)
