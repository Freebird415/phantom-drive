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

rem ---- 2. 展示将要丢弃的内容 ----
echo.
echo [2/3] 以下是你本地与最新版本的差异：
chcp 65001 >nul
git status --short
echo --------------------------------------------
git log origin/main..HEAD --oneline
chcp 936 >nul
echo.
echo   （?? 开头的是新增文件，恢复不会删除它们；其余改动会被还原）
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

if not defined dirty if not defined ahead goto :nothing_to_do
if defined ahead goto :warn_ahead
goto :confirm

:nothing_to_do
echo   本地已经和 GitHub 上的最新版本一致，没有需要恢复的改动。
if defined untracked (
    echo   （检测到新增的未跟踪文件，恢复操作不会影响它们）
)
echo.
pause
exit /b 0

:warn_ahead
echo   [注意] 上面列表里有"本地未上传的提交"，恢复后它们会消失！
echo.
goto :confirm

:confirm
echo   确认要丢弃以上全部改动、恢复成 GitHub 上的最新版本吗？
set /p ok="请输入 YES 确认（其他任意输入 = 取消）："
if /i not "%ok%"=="YES" (
    echo   已取消，什么都没有改动。
    echo.
    pause
    exit /b 0
)

rem ---- 3. 执行恢复 ----
echo.
echo [3/3] 正在恢复……
chcp 65001 >nul
git reset --hard origin/main
chcp 936 >nul
if errorlevel 1 (
    echo.
    echo [错误] 恢复失败，请把本窗口截图发到群里。
    echo.
    pause
    exit /b 1
)

echo.
echo ============================================
echo   [完成] 已恢复到 GitHub 上的最新版本。
echo ============================================
echo.
pause
exit /b 0
