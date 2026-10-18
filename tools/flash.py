# -*- coding: utf-8 -*-
"""一键烧录（ST-Link / SWD），失败自动换策略重试并给出中文原因；ST-Link 坏了走 UART。

    python tools/flash.py                    # 编译 + 烧录（失败逐级降速重试 + 诊断）
    python tools/flash.py --erase            # 先整片擦除（= Keil 的 Erase Full Chip）
    python tools/flash.py --hold-reset       # 按住核心板 R 键连接（无 NRST 时的穷人版）
    python tools/flash.py --uart COM5        # 走 UART(ISP) 烧录，完全绕开 ST-Link
    python tools/flash.py --uart-erase COM5  # 走 UART 只做整片擦除（= 解锁 RDP）
    python tools/flash.py --build-only       # 只编译

为什么需要它：`pio run -t upload` 失败只给一句
`Error: init mode failed (unable to connect to the target)`，
不告诉你是"车没上电 / SWD 线反了 / 探针切不进调试模式 / 板子被读保护"。
"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
FW = ROOT / "firmware"
BUILD = FW / ".pio" / "build" / "bluepill_f103c8"
ELF = BUILD / "firmware.elf"
BIN = BUILD / "firmware.bin"

VOLT_RE = re.compile(r"Target voltage:\s*([0-9.]+)")
LINK_RE = re.compile(r"STLINK\s+(\S+)")


def _fix_console() -> None:
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(errors="replace")     # type: ignore[attr-defined]
        except Exception:
            pass


def _tool(name: str) -> str | None:
    p = shutil.which(name)
    if p:
        return p
    for cand in (Path.home() / ".platformio" / "penv" / "Scripts" / (name + ".exe"),
                 Path.home() / ".platformio" / "penv" / "Scripts" / name):
        if cand.exists():
            return str(cand)
    return None


def pio() -> str:
    exe = _tool("pio") or _tool("platformio")
    if not exe:
        raise SystemExit("[x] 找不到 pio，先装 PlatformIO，或用仓库根 工具箱.bat 菜单 B。")
    return exe


def openocd() -> tuple[str, Path] | None:
    base = Path.home() / ".platformio" / "packages" / "tool-openocd"
    exe = base / "bin" / "openocd.exe"
    scripts = base / "openocd" / "scripts"
    if exe.exists() and scripts.exists():
        return str(exe), scripts
    return None


def build() -> bool:
    print("[1/2] 编译 ...", flush=True)
    r = subprocess.run([pio(), "run", "-d", str(FW)], capture_output=True, text=True,
                       errors="replace")
    for ln in [x for x in (r.stdout or "").splitlines() if x.strip()][-6:]:
        print("      " + ln)
    if r.returncode != 0:
        blob = (r.stdout or "") + (r.stderr or "")
        if "check_size" in blob or "snapshot area" in blob:
            print("\n[x] 编译失败：固件越过快照保留区 0x0800D800（54KB 红线）。")
        else:
            print("\n[x] 编译失败（不是烧录问题）。")
        return False
    if BIN.exists():
        print(f"      固件：{BIN.name}  {BIN.stat().st_size} B")
    return True


def classify(text: str) -> str:
    t = text.lower()
    m = VOLT_RE.search(text)
    v = float(m.group(1)) if m else None
    if "status code 0x44" in t:
        return ("探针能通信、能读电压，但切不进 SWD 调试模式（status 0x44）。"
                "a) 拔插 ST-Link 的 USB 等 5 秒再试；b) 仍不行就是探针 SWD 输出级坏了"
                " ⇒ 改用 UART 烧录（python tools/flash.py --uart COMx）。")
    if "init mode failed" in t or "unable to connect to the target" in t:
        if v is not None and v < 2.0:
            return (f"目标电压只有 {v:.2f} V ⇒ 车没上电（电池没接 / 总开关没开）。"
                    "先给车上电，量到 3V3≈3.3V 再试。")
        return ("探针在、目标也有电，但 SWD 握不上：a) SWDIO/SWCLK 接反（最常见）"
                " b) GND 没接 c) 线松 d) 车断电、只用 ST-Link 供 3V3 再试"
                " e) 按核心板 R 键用 --hold-reset，或用 --uart 走串口。")
    if "no device found" in t or "libusb" in t or "open failed" in t:
        return ("ST-Link 没被系统识别：拔插 USB；关掉 STM32CubeProgrammer / Keil / "
                "ST-LINK Utility 等占用探针的软件；换 USB 口。")
    if "read protected" in t or "option bytes" in t or "RDP" in text:
        return ("检测到读保护（RDP）。用 --erase（整片擦除解锁）或 --uart-erase（串口解锁）。")
    if "verify" in t and ("fail" in t or "mismatch" in t):
        return "写入后校验失败 ⇒ 供电不稳 / 接触不良，重新上电、压紧 SWD 后再试。"
    if "timeout" in t or "timed out" in t:
        return "通信超时 ⇒ 速度太快或接线差；脚本会自动降速重试。"
    if "scan chain" in t or "examination" in t:
        return "SWD 扫描失败 ⇒ 信号线没通（接反/松动/缺 GND）。"
    return "未识别的失败（原文见下）。"


def show_probe() -> None:
    oc = openocd()
    if not oc:
        return
    exe, scripts = oc
    try:
        r = subprocess.run(
            [exe, "-d3", "-s", str(scripts), "-f", "interface/stlink.cfg",
             "-c", "transport select swd", "-f", "target/stm32f1x.cfg",
             "-c", "reset_config none", "-c", "init; shutdown"],
            capture_output=True, text=True, errors="replace", timeout=60)
        blob = (r.stdout or "") + (r.stderr or "")
        m, v = LINK_RE.search(blob), VOLT_RE.search(blob)
        if m:
            print(f"      探针：STLINK {m.group(1)}")
        else:
            print("      !! 没检测到 ST-Link（USB / 驱动 / 被占用）!!")
        if v:
            iv = float(v.group(1))
            print(f"      目标电压：{iv:.2f} V  ({'车已上电 OK' if iv > 2.5 else '!! 目标没电 !!'})")
        if "status code 0x44" in blob:
            print("      探针健康：USB/固件 OK、能读电压，但切不进调试模式 (0x44)")
            print("      -> 先拔插 ST-Link 的 USB 等 5 秒；仍不行=探针 SWD 输出级坏了")
            print("      -> 急用：--uart COMx 走串口烧录（BOOT0=1 + USB-TTL）")
        if "cortex-m3" in blob.lower():
            print("      ✅ 目标是响应 SWD 的（init 成功）")
    except Exception:
        pass


def _oc_run(extra: list[str], speed: int | None = None,
            assert_srst: bool = False) -> tuple[bool, str]:
    oc = openocd()
    if not oc:
        return False, "openocd 不可用"
    exe, scripts = oc
    args = [exe, "-d3", "-s", str(scripts), "-f", "interface/stlink.cfg",
            "-c", "transport select swd"]
    if speed:
        args += ["-c", f"adapter speed {speed}"]
    args += ["-f", "target/stm32f1x.cfg"]
    if assert_srst:
        args += ["-c", "reset_config srst_only srst_nogate connect_assert_srst"]
    else:
        args += ["-c", "reset_config none"]
    args += extra
    r = subprocess.run(args, capture_output=True, text=True, errors="replace")
    return r.returncode == 0, (r.stdout or "") + (r.stderr or "")


def try_openocd(speed: int | None, assert_srst: bool = False) -> tuple[bool, str]:
    return _oc_run(["-c", f"program {{{ELF}}} verify reset; shutdown;"], speed, assert_srst)


def try_mass_erase() -> tuple[bool, str]:
    """= Keil 的 Erase Full Chip（顺带解除 RDP 读保护）。注意：它也需要先连上 SWD。"""
    return _oc_run(["-c", "init; reset halt; stm32f1x mass_erase 0; shutdown"])


def try_unlock() -> tuple[bool, str]:
    """stm32f1x unlock 0：解除读保护（会触发整片擦除）。"""
    return _oc_run(["-c", "init; stm32f1x unlock 0; shutdown"])


def try_hold_reset(seconds: int = 6) -> tuple[bool, str]:
    """无 NRST 时的"穷人版 connect-under-reset"：倒计时期间按住核心板 R 键。"""
    print(f"      >>> 现在【按住核心板的 R 复位键不放】，{seconds} 秒后自动连接 <<<", flush=True)
    for k in range(seconds, 0, -1):
        print(f"          {k} ...", flush=True)
        time.sleep(1)
    ok, blob = _oc_run(["-c", f"program {{{ELF}}} verify reset; shutdown;"], 100)
    print("      >>> 可以松开复位键了 <<<", flush=True)
    return ok, blob


def kill_stale() -> None:
    for name in ("openocd.exe", "openocd"):
        try:
            subprocess.run(["taskkill", "/F", "/IM", name], capture_output=True, text=True)
        except Exception:
            pass


def uart_flash(port: str, erase_only: bool = False) -> int:
    """UART(ISP)：走芯片 ROM bootloader，完全不依赖 SWD / NRST / ST-Link。"""
    exe = Path.home() / ".platformio" / "packages" / "tool-stm32flash" / "stm32flash.exe"
    if not exe.exists():
        print("[x] 没找到 stm32flash（PlatformIO 的 tool-stm32flash 包）")
        return 1
    if not BIN.exists():
        print("[x] 还没编译，先跑 python tools/flash.py --build-only")
        return 1
    args = [str(exe), "-b", "115200"]
    if erase_only:
        args += ["-o"]
    else:
        args += ["-w", str(BIN), "-v", "-g", "0x08000000"]
    args.append(port)
    print("[UART] " + " ".join(args))
    r = subprocess.run(args, text=True, errors="replace")
    if r.returncode == 0:
        print("[OK] UART 通道成功。把 BOOT0 跳回 0，断电再上电（否则一直停在 bootloader）。")
        return 0
    print("[x] UART 失败：查 ① BOOT0 是否=1 ② 是否刚上电 ③ TX/RX 交叉（板TX→TTL RX）④ GND 共地")
    return 2


def wait_enter(msg: str) -> None:
    try:
        input(msg)
    except (EOFError, KeyboardInterrupt):
        pass


def main() -> int:
    _fix_console()
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-only", action="store_true")
    ap.add_argument("--no-wait", action="store_true", help="失败后不等人按回车")
    ap.add_argument("--erase", action="store_true", help="先整片擦除（= Erase Full Chip / 解 RDP）")
    ap.add_argument("--hold-reset", action="store_true", help="按住核心板 R 键连接")
    ap.add_argument("--uart", metavar="COMx", help="走 UART(ISP) 烧录（需 BOOT0=1）")
    ap.add_argument("--uart-erase", metavar="COMx", help="走 UART 只做整片擦除（解锁用）")
    ap.add_argument("--probe", action="store_true",
                    help="只测探针能否连上芯片（约 1 秒；改了接线就用它快速复测）")
    a = ap.parse_args()

    print("=" * 66)
    print(" 工创小车 - 烧录工具")
    print("=" * 66)
    if a.probe:
        show_probe()
        ok, blob = _oc_run(["-c", "init; shutdown"])
        if ok:
            print("[OK] SWD 能连上芯片了 => 现在可以正式烧录")
        else:
            print("[x] SWD 仍连不上：")
            for ln in classify(blob).splitlines():
                print("    " + ln)
        return 0 if ok else 2
    if not build():
        return 1
    if a.uart or a.uart_erase:
        return uart_flash(a.uart or a.uart_erase, erase_only=bool(a.uart_erase))
    if a.build_only:
        print("[OK] 只编译完成。")
        return 0

    print("\n[2/2] 烧录 ...")
    show_probe()

    while True:
        plan: list[tuple[str, object]] = []
        if a.hold_reset:
            plan.append(("⓪ 按住 R 复位键连接", lambda: try_hold_reset(6)))
        if a.erase:
            plan.append(("⓪ 解除读保护 unlock（会整片擦除）", try_unlock))
            plan.append(("⓪ 整片擦除 mass_erase", try_mass_erase))
        plan += [
            ("① 默认速度", lambda: try_openocd(None)),
            ("② 100kHz", lambda: try_openocd(100_000)),
            ("③ 48kHz", lambda: try_openocd(48_000)),
            ("④ 10kHz", lambda: try_openocd(10_000)),
            ("⑤ 100kHz + connect_assert_srst（需 NRST）",
             lambda: try_openocd(100_000, assert_srst=True)),
            ("⑥ 100kHz + reset halt 先抓 CPU",
             lambda: _oc_run(["-c", "reset halt",
                              "-c", f"program {{{ELF}}} verify reset; shutdown;"], 100_000)),
        ]
        for label, fn in plan:
            print(f"\n--- 尝试 {label} ---")
            kill_stale()
            time.sleep(0.4)
            ok, blob = fn()          # type: ignore[operator]
            if ok:
                print("\n[OK] 成功 ✓")
                if a.erase or a.hold_reset:
                    print("     （擦除/解锁成功，接着重跑一次不带 --erase 的烧录）")
                else:
                    print("     下一步：轮子架空、TRACE 1 试转，再落地。")
                return 0
            for ln in classify(blob).splitlines():
                print("    " + ln)
            raw = [ln.strip() for ln in blob.splitlines()
                   if ln.strip().startswith(("Error", "Warn", "** "))]
            if raw:
                print("    原文：" + raw[-1][:150])

        if a.no_wait:
            print("\n[x] 全部失败（--no-wait）。")
            return 2
        print("\n[x] 全部失败。物理处理顺序：")
        print("    1) 车彻底断电，只用 ST-Link 的 3V3 给核心板供电（四根线全接）再试")
        print("    2) 拔插 ST-Link USB 等 5 秒；关掉 CubeProgrammer / Keil")
        print("    3) 对调 SWDIO 与 SWCLK；确认 GND 共地")
        print("    4) 看核心板 PC13 心跳灯是否 2Hz 闪（不闪=MCU 没跑）")
        print("    5) 用 --hold-reset（按住 R 键）或 --erase（整片擦除/解锁）")
        print("    6) 救命通道：--uart COMx（BOOT0=1 + USB-TTL，完全不依赖 ST-Link）")
        wait_enter("\n    处理后按【回车】重试（Ctrl+C 退出）：")


if __name__ == "__main__":
    raise SystemExit(main())
