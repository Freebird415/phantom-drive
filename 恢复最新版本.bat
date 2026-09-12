@echo off
chcp 936 >nul
title 恢复最新版本 - phantom-drive

rem ============================================================
rem  恢复最新版本：丢弃本地所有改动，回到 GitHub 上的最新状态
rem  适用：不小心改坏 / 删错了文件，想撤回
rem  注意：本地未上传的提交也会被丢弃；新增的未跟踪文件会保留
rem ============================================================

cd /d "%~dp0"

echo.
echo ============================================
echo    恢复最新版本（丢弃本地改动）
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

set "branch="
for /f "delims=" %%i in ('git symbolic-ref --short HEAD') do set "branch=%%i"
if /i not "%branch%"=="main" (
    echo [提示] 你当前在分支 "%branch%" 上，不是 main 分支。
    echo 本脚本只用于恢复 main 分支，请先联系组长处理。
    echo.
    pause
    exit /b 1
)

rem ---- 1. 获取 GitHub 最新版本信息 ----
echo [1/3] 正在获取 GitHub 上的最新版本……
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

rem ---- 2. 展示差异并选择操作 ----
echo.
echo [2/3] 以下是你本地与最新版本的差异：
chcp 65001 >nul
git status --short
echo --------------------------------------------
git log origin/main..HEAD --oneline
chcp 936 >nul
echo.

set "dirty="
git diff --quiet
if errorlevel 1 set "dirty=1"
git diff --cached --quiet
if errorlevel 1 set "dirty=1"
set "untracked="
for /f "delims=" %%i in ('git ls-files --others --exclude-standard') do set "untracked=1"
set "ahead="
for /f "delims=" %%i in ('git log origin/main..HEAD --oneline') do set "ahead=1"

if not defined dirty if not defined ahead if not defined untracked goto :nothing_to_do

if defined untracked (
    echo   新增的文件：
    chcp 65001 >nul
    git ls-files --others --exclude-standard
    chcp 936 >nul
    echo.
)
if defined ahead (
    echo   [注意] 上面列表里有"本地未上传的提交"，恢复后它们会消失！
    echo.
)

echo   请选择操作：
echo     1 = 还原已跟踪的改动（新增文件保留）  ← 推荐
echo     2 = 完全恢复（新增文件也删除）
echo     0 = 取消
echo.
:choose_action
set "act="
set /p act="请输入（1/2/0）："
if "%act%"=="0" (
    echo   已取消，什么都没有改动。
    echo.
    pause
    exit /b 0
)
if "%act%"=="1" goto :do_restore
if "%act%"=="2" goto :do_restore
echo   [错误] 请输入 1、2 或 0。
goto :choose_action

:nothing_to_do
echo   本地已经和 GitHub 上的最新版本完全一致，没有任何需要恢复的东西。
echo.
pause
exit /b 0

:do_restore
echo.
echo [3/3] 正在恢复……
chcp 65001 >nul
rem 先取消暂存：保证"git add 过但没提交"的新文件也能按选择处理
git reset -q
git reset --hard origin/main
if errorlevel 1 goto :restore_fail
if "%act%"=="2" (
    git clean -fd
    if errorlevel 1 goto :restore_fail
)
chcp 936 >nul
echo.
echo ============================================
if "%act%"=="2" (
    echo   [完成] 已完全恢复（新增文件已删除）
) else (
    echo   [完成] 已还原到最新版本（新增文件已保留）
)
echo ============================================
echo.
pause
exit /b 0

:restore_fail
chcp 936 >nul
echo.
echo [错误] 恢复失败，请把本窗口截图发到群里。
echo.
pause
exit /b 1
