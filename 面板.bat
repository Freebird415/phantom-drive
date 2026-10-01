@echo off
chcp 936 >nul
title 工创小车 - 一键面板
cd /d "%~dp0"

rem ============================================================
rem  工创小车 · 一键面板（给不想敲命令的人用）
rem  只需一个常识：要烧录/采集时，USB-TTL 要插在电脑上
rem  GBK 编码，直接 Windows 双击运行，不需要改任何设置
rem ============================================================

where python >nul 2>&1
if errorlevel 1 (
    echo [错误] 没有检测到 Python。
    echo 请先安装：https://www.python.org/downloads/  （安装时勾选 Add to PATH）
    echo 然后执行一次：python -m pip install pyserial
    echo.
    pause
    exit /b 1
)

:menu
cls
echo ============================================
echo    工创小车  ·  一键面板
echo ============================================
echo.
echo    1   一键实验：烧录 + 采集 + 出图   ^<-- 最常用
echo    2   只采集，不烧录（车已经在跑）
echo    3   看串口原始输出（Ctrl+C 退出）
echo    4   打开最新一次实验日志
echo    5   打开算法代码文件夹（app/）
echo    6   用 VS Code 打开固件工程
echo    7   查看电脑上有哪些串口
echo    8   拉取最新代码（git pull）
echo    9   提交我的改动（走 提交.bat）
echo    0   退出
echo.
echo   [提醒] 烧录/采集前请确认：USB-TTL 已插好；如果要让车动，先把轮子架空！
echo.
set "act="
set /p act="请输入序号后回车："
if "%act%"=="1" goto :exp
if "%act%"=="2" goto :exp_nf
if "%act%"=="3" goto :raw
if "%act%"=="4" goto :opendir
if "%act%"=="5" goto :opencode
if "%act%"=="6" goto :vscode
if "%act%"=="7" goto :ports
if "%act%"=="8" goto :pull
if "%act%"=="9" goto :commit
if "%act%"=="0" exit /b 0
goto :menu

:exp
call :ask
echo.
echo [提示] 下面会先编译烧录再采集。若车会动，请先确认轮子已架空！
echo.
pause
python tools\run.py --note "%note%" --seconds %sec%
echo.
pause
goto :menu

:exp_nf
call :ask
echo.
python tools\run.py --no-flash --note "%note%" --seconds %sec%
echo.
pause
goto :menu

:ask
set "note="
set /p note="一句话描述（这轮做了什么/什么现象，直接回车 = 未命名）："
if "%note%"=="" set "note=未命名"
set "sec="
set /p sec="采集多少秒（直接回车 = 30）："
if "%sec%"=="" set "sec=30"
exit /b 0

:raw
echo.
echo 正在打开串口（Ctrl+C 可退出）...
python tools\run.py --raw --seconds 3600
echo.
pause
goto :menu

:opendir
powershell -NoProfile -Command "$d = Get-ChildItem 'logs' -Directory | Sort-Object Name -Descending | Select-Object -First 1; if ($d) { explorer $d.FullName; Write-Host ('已打开：' + $d.Name) } else { Write-Host '还没有实验日志' }"
echo.
pause
goto :menu

:opencode
explorer "firmware\src\app"
goto :menu

:vscode
where code >nul 2>&1
if errorlevel 1 (
    echo.
    echo [提示] 没找到 VS Code 的 code 命令。
    echo 请手动打开 VS Code，然后选择 文件 - 打开文件夹 - 选 firmware 目录。
    echo.
    pause
    goto :menu
)
start "" code "firmware"
goto :menu

:ports
echo.
python tools\run.py --ports
echo.
pause
goto :menu

:pull
echo.
call "拉取.bat"
goto :menu

:commit
echo.
call "提交.bat"
goto :menu
