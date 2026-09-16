@echo off
chcp 936 >nul
title 拉取 - phantom-drive

rem ============================================================
rem  拉取云端最新版本（本地未上传的改动会被丢弃）
rem  正确用法：每次开工前先双击本脚本，再开始改文件
rem  GBK 编码，中文 Windows 双击即用，不需要敲任何命令
rem ============================================================

cd /d "%~dp0"

echo.
echo ============================================
echo    phantom-drive 拉取（与云端同步）
echo ============================================
echo.

rem ---- 0. 环境检查 ----
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
    echo 请确认本脚本放在 phantom-drive 仓库文件夹里。
    echo.
    pause
    exit /b 1
)

rem ---- 0.1 分支检查：只允许 main ----
set "branch="
chcp 65001 >nul
for /f "delims=" %%i in ('git symbolic-ref --short HEAD') do set "branch=%%i"
chcp 936 >nul
if /i not "%branch%"=="main" (
    echo [提示] 你当前在分支“%branch%”上，不是 main 分支。
    echo 本脚本只用于同步 main 分支，请先联系组长处理。
    echo.
    pause
    exit /b 1
)

git config core.quotepath false >nul 2>&1

rem ---- 1. 取回云端最新 ----
echo [1/3] 正在连接 GitHub 获取最新版本……
chcp 65001 >nul
git fetch origin
if errorlevel 1 (
    chcp 936 >nul
    echo.
    echo [错误] 连接 GitHub 失败（网络问题？）。
    echo 请检查网络后重试；搞不定就把本窗口截图发到群里。
    echo.
    pause
    exit /b 1
)
chcp 936 >nul

rem ---- 2. 检查会被丢弃的东西 ----
set "dirty="
set "ahead="
chcp 65001 >nul
for /f "delims=" %%i in ('git status --porcelain') do set "dirty=1"
for /f "delims=" %%i in ('git log origin/main..HEAD --oneline') do set "ahead=1"
chcp 936 >nul

if not defined dirty if not defined ahead goto :do_sync

echo.
echo [2/3] 检测到你本地还有没上传到云端的东西：
echo.
echo --------------------------------------------
echo 本地还没上传的提交：
chcp 65001 >nul
git status --short
git log origin/main..HEAD --oneline
chcp 936 >nul
echo.
echo   [注意] 继续的话，上面这些内容会被云端版本覆盖、永久消失！
echo          如果其中有你要保留的活，请先关闭本窗口，双击「提交.bat」上传。
echo.
echo     1 = 继续同步（丢弃本地改动）
echo     0 = 取消
echo.
set "act="
set /p act="请输入（1/0）："
if not "%act%"=="1" (
    echo.
    echo   已取消，什么都没有改动。
    echo.
    pause
    exit /b 0
)

:do_sync
echo.
echo [3/3] 正在与云端同步……
chcp 65001 >nul
git reset -q
git reset --hard origin/main
git clean -fd
if errorlevel 1 (
    chcp 936 >nul
    echo.
    echo [错误] 同步失败，请把本窗口截图发到群里。
    echo.
    pause
    exit /b 1
)
chcp 936 >nul
echo.
echo 当前版本：
chcp 65001 >nul
git log -1 --oneline
chcp 936 >nul

echo.
echo ============================================
echo   [完成] 已与云端完全同步
echo   现在可以改文件了；改完双击「提交.bat」上传
echo ============================================
echo.
pause
exit /b 0
