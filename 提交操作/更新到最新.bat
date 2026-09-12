@echo off
chcp 936 >nul
title 更新到最新 - phantom-drive

rem ============================================================
rem  只拉取最新文件，不提交任何东西
rem  适合：开工前同步、只想看别人的更新
rem ============================================================

cd /d "%~dp0.."

echo.
echo ============================================
echo    拉取最新版本
echo ============================================
echo.

where git >nul 2>&1
if errorlevel 1 (
    echo [错误] 没有检测到 Git。
    echo 请先安装：https://git-scm.com/download/win
    echo.
    pause
    exit /b 1
)

git rev-parse --is-inside-work-tree >nul 2>&1
if errorlevel 1 (
    echo [错误] 当前文件夹不是 Git 仓库。
    echo 请确认你是双击的 phantom-drive 文件夹里的脚本。
    echo.
    pause
    exit /b 1
)

echo 正在拉取……
chcp 65001 >nul
git pull --no-edit
if errorlevel 1 (
    chcp 936 >nul
    echo.
    echo [错误] 拉取失败。
    echo 可能原因：网络问题，或你本地有没提交的改动挡住了更新。
    echo 如果是后者，先运行「提交更改.bat」把改动交上去，再运行本脚本。
    echo 搞不定就把本窗口截图发到群里。
    echo.
    pause
    exit /b 1
)
chcp 936 >nul

echo.
echo ============================================
echo   [完成] 你的文件已经是最新版本。
echo ============================================
echo.
pause
exit /b 0
