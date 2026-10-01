@echo off
chcp 936 >nul
title 提交 - phantom-drive

rem ============================================================
rem  提交并上传（只负责提交，不负责拉取）
rem  正确用法：开工前先双击「拉取.bat」，改完文件再双击本脚本
rem  GBK 编码，中文 Windows 双击即用，不需要敲任何命令
rem ============================================================

rem ---- 切到仓库根目录（本脚本就放在仓库根目录）----
cd /d "%~dp0"

echo.
echo ============================================
echo    phantom-drive 提交
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

if exist ".git\MERGE_HEAD" (
    echo [错误] 仓库目前处于“冲突未解决”状态。
    echo 请不要继续操作，直接找组长处理。
    echo.
    pause
    exit /b 1
)

rem ---- 0.1 身份检查：第一次运行引导填写并保存 ----
set "uname="
set "uemail="
chcp 65001 >nul
for /f "delims=" %%i in ('git config user.name') do set "uname=%%i"
for /f "delims=" %%i in ('git config user.email') do set "uemail=%%i"
chcp 936 >nul
if not defined uname goto :ask_identity
if not defined uemail goto :ask_identity
goto :identity_ready

:ask_identity
echo [首次使用] 需要先填写你的提交身份（只需填一次，以后自动记住）。
echo    姓名：填你的真名（如 王政淋），会显示在提交记录里
echo    邮箱：填注册 GitHub 用的邮箱，用于把你的提交关联到你的账号
echo.
:ask_name
set "gname="
set /p gname="请输入姓名："
if not defined gname (
    echo    姓名不能为空，请重新输入。
    goto :ask_name
)
:ask_email
set "gemail="
set /p gemail="请输入邮箱："
if not defined gemail (
    echo    邮箱不能为空，请重新输入。
    goto :ask_email
)
git config --global user.name "%gname%"
git config --global user.email "%gemail%"
rem 重新从 git 读回（读取时必须切到 UTF-8，否则中文会被双重转换）
chcp 65001 >nul
set "uname="
for /f "delims=" %%i in ('git config user.name') do set "uname=%%i"
set "uemail="
for /f "delims=" %%i in ('git config user.email') do set "uemail=%%i"
chcp 936 >nul
echo.
echo    已保存，以后不用再填。
echo.

:identity_ready
echo    当前提交身份：%uname%（%uemail%）
echo.

git config pull.rebase false >nul 2>&1
git config core.quotepath false >nul 2>&1

rem ---- 1. 显示改动（文件清单 + 行数统计 + 新增文件）----
echo [1/4] 你本地的改动：
chcp 65001 >nul
git status --short
chcp 936 >nul
echo.
echo ---- 行数统计（已跟踪的文件）----
chcp 65001 >nul
git diff --stat HEAD
chcp 936 >nul

set "has_changes="
for /f "delims=" %%i in ('git status --porcelain') do set "has_changes=1"
if not defined has_changes (
    echo.
    echo   没有检测到任何改动 —— 没有东西需要提交。
    echo   如果你是想同步云端最新版本，请双击「拉取.bat」。
    echo.
    pause
    exit /b 0
)

set "untracked="
chcp 65001 >nul
for /f "delims=" %%i in ('git ls-files --others --exclude-standard') do set "untracked=1"
chcp 936 >nul
if not defined untracked goto :no_untracked
echo.
echo ---- 新增的文件（会被一起提交）----
chcp 65001 >nul
git ls-files --others --exclude-standard
chcp 936 >nul
:no_untracked
echo.

rem ---- 2. 提醒页：先确认本次已拉取 ----
echo [2/4] 提交前提醒：本次开工前运行过「拉取.bat」吗？
echo.
echo     · 拉取过（本地就是最新版本）  →  输入 Y 继续
echo     · 没拉取，但手里有没提交的活   →  也输入 Y（脚本会自动合并云端，你的活不会丢）
echo     · 只想同步云端、手里没有活     →  输入 N 退出，去双击「拉取.bat」
echo.
:ask_pulled
set "pulled="
set /p pulled="继续提交？(Y/N)："
if /i "%pulled%"=="Y" goto :pulled_ok
if /i "%pulled%"=="N" (
    echo.
    echo   已取消提交，什么都没有改动。
    echo   手里有没提交的活 → 重新双击本脚本、输入 Y（会自动合并，不会丢）；
    echo   只想同步云端     → 双击「拉取.bat」（它用于开工前，会覆盖本地未提交的改动）。
    echo.
    pause
    exit /b 0
)
echo [错误] 请输入 Y 或 N。
goto :ask_pulled

:pulled_ok
echo.

rem ---- 3. 填写提交说明 ----
echo [3/4] 请选择这次提交的类型：
echo    1 = feat   新功能 / 新代码
echo    2 = fix    修改错误
echo    3 = docs   文档
echo    4 = hw     硬件（电路 / PCB）
echo    5 = mech   机械（车架 / 模型）
echo    6 = tools  脚本 / 工具
echo.
:choose_type
set "ctype="
set /p ctype="请输入序号（1-6）："

set "t="
if "%ctype%"=="1" set "t=feat"
if "%ctype%"=="2" set "t=fix"
if "%ctype%"=="3" set "t=docs"
if "%ctype%"=="4" set "t=hw"
if "%ctype%"=="5" set "t=mech"
if "%ctype%"=="6" set "t=tools"
if not defined t (
    echo [错误] 序号无效，请重新输入（或直接关闭本窗口取消）。
    goto :choose_type
)

:input_desc
set "desc="
set /p desc="请用一句话描述你做了什么："
if "%desc%"=="" (
    echo [错误] 描述不能为空，请重新输入。
    goto :input_desc
)

set "msg=%t%: %desc%"
echo.
echo   将要提交： %msg%
echo.
set /p ok="确认提交并上传？(Y/N)："
if /i not "%ok%"=="Y" (
    echo   已取消，什么都没有提交。
    echo.
    pause
    exit /b 0
)

rem ---- 3. 提交并上传 ----
echo.
echo [4/4] 正在提交并上传……
chcp 65001 >nul
git add -A
git commit -m "%msg%"
if errorlevel 1 (
    chcp 936 >nul
    echo.
    echo [错误] 提交失败，请截图发到群里。
    echo.
    pause
    exit /b 1
)

git push
if errorlevel 1 (
    chcp 936 >nul
    echo.
    echo [提示] 上传被拒绝 —— 云端有别人的新改动（你这次开工前可能忘了先拉取）。
    echo        正在自动合并云端版本，请稍等……
    chcp 65001 >nul
    git pull --no-edit
    if errorlevel 1 (
        chcp 936 >nul
        echo.
        echo [错误] 自动合并失败（可能冲突）。请把本窗口截图发到群里，不要自己乱试。
        echo.
        pause
        exit /b 1
    )
    git push
    if errorlevel 1 (
        chcp 936 >nul
        echo.
        echo [错误] 上传仍然失败。请把本窗口截图发到群里。
        echo.
        pause
        exit /b 1
    )
)

chcp 936 >nul
echo.
echo ============================================
echo   [完成] 已上传成功！
echo   可以打开 GitHub 网页确认你的提交。
echo   下次开工前记得先双击「拉取.bat」。
echo ============================================
echo.
pause
exit /b 0
