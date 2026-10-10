@echo off
chcp 936 >nul
cd /d "%~dp0"

rem ============================================================
rem  工创小车 · 蓝牙日志界面（网页版）
rem  双击 => 后台起服务 + 自动打开浏览器；本窗口会自己消失
rem  停止服务：把网页关掉即可（还在记录会先自动保存，然后服务自己退出）
rem  要看报错信息：在命令行运行  python tools/webview.py  （带终端窗口的版本）
rem  GBK 编码，直接 Windows 双击运行，不需要改任何设置
rem ============================================================

where py >nul 2>&1
if errorlevel 1 (
    echo [错误] 没有检测到 Python。
    echo 请先安装：https://www.python.org/downloads/  （安装时勾选 Add to PATH）
    pause
    exit /b 1
)

py -3 -c "import bleak" >nul 2>&1
if errorlevel 1 (
    echo [错误] 缺少蓝牙依赖 bleak。
    echo 请执行一次：py -3 -m pip install bleak
    pause
    exit /b 1
)

rem 后台启动（pythonw = 无终端窗口）；端口已占用时 webview 自己会打开浏览器
start "" pyw -3 tools/webview.py
exit /b 0
