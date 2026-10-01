"""读板载快照（"黑匣子"）—— 跑车的时候插不了串口，跑完用这个把数据取回来

用法（在仓库根目录执行）：
    python tools/snapshot.py --note "A4 直线 kp10 v15"
    python tools/snapshot.py --note "..." --port COM11 --no-flash
    python tools/snapshot.py --status          # 只看板子上记了多少帧
    python tools/snapshot.py --erase           # 清空板载记录

流程内幕：
    给固件发 `DUMP` → 板子把 RAM 里的快照以 **与 $T 完全相同** 的格式吐出来
    → 本脚本截取「# DUMP n= / # DUMP END」之间的行 → 存成 telemetry.csv
    → CSV 列与 tools/telemetry.py 一致 ⇒ **plot.py 可以直接画图** ✓

⚠️ 固件侧前提（2026-09-30 加的）：
    · 发车前 **长按 PA12 按钮 1 秒** → 开始记录 + 发车
    · 停车 **再长按 1 秒** → 停车 + 停止记录
    · **必须先停车再断电** —— RAM 快照断电就丢！
"""
from __future__ import annotations

import argparse
import csv
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import telemetry  # noqa: E402

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("缺依赖：python -m pip install pyserial")

DUMP_BEGIN = re.compile(r"^#\s*DUMP\s+n=(\d+)")
DUMP_END = "# DUMP END"


def pick_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    for p in list_ports.comports():
        desc = (p.description or "").lower()
        if any(k in desc for k in ("ch340", "cp210", "usb-serial", "usb serial")):
            return p.device
    sys.exit("找不到 USB-TTL。用 --port 指定，或 `python tools/run.py --ports` 看有哪些串口。")


def cmd(ser, text: str, wait: float = 0.6, quiet: bool = True) -> str:
    ser.reset_input_buffer()
    ser.write((text + "\r\n").encode())
    t0 = time.time()
    buf = b""
    while time.time() - t0 < wait:
        buf += ser.read(600)
    out = buf.decode("utf-8", "replace")
    if not quiet:
        print(out, end="")
    return out


def show_status(ser):
    for line in cmd(ser, "SNAP?", 0.6).splitlines():
        if line.strip() and not line.startswith(("$T", "$U", "$P")):
            print("   ", line)
    for line in cmd(ser, "GR", 0.8).splitlines():
        if line.startswith("GR>"):
            print("   ", line)


def main():
    ap = argparse.ArgumentParser(description="读板载 RAM 快照（黑匣子）")
    ap.add_argument("--note", default="snapshot", help="一句话描述（会变成日志目录名）")
    ap.add_argument("--port", help="串口号，默认自动选 USB-TTL")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--status", action="store_true", help="只看状态后退出")
    ap.add_argument("--erase", action="store_true", help="清空板载记录后退出")
    ap.add_argument("--timeout", type=float, default=60.0, help="DUMP 最长等待秒数")
    args = ap.parse_args()

    port = pick_port(args.port)
    print(f"USB-TTL = {port}")
    ser = serial.Serial(port, args.baud, timeout=0.2)
    time.sleep(0.4)
    cmd(ser, "AUTO 0", 0.4)          # 关掉超声波自动测量，输出干净

    if args.status:
        show_status(ser)
        ser.close()
        return
    if args.erase:
        cmd(ser, "SNAPRESET", 0.5, quiet=False)
        ser.close()
        return

    # ---- 发 DUMP，收集到 # DUMP END ----
    print("[1/3] 发送 DUMP，读板载快照…")
    ser.reset_input_buffer()
    ser.write(b"DUMP\r\n")
    t0 = time.time()
    lines: list[str] = []
    n_expect = None
    while time.time() - t0 < args.timeout:
        raw = ser.readline().decode("utf-8", "replace").rstrip()
        if not raw:
            continue
        if n_expect is None:
            m = DUMP_BEGIN.match(raw)
            if m:
                n_expect = int(m.group(1))
                print(f"      板子上有 {n_expect} 帧（≈{n_expect/100:.1f}s @100Hz）")
                continue
            continue                     # DUMP 之前的实时 $T 一律丢掉
        if raw.startswith(DUMP_END):
            break
        if raw.startswith("$T,"):
            lines.append(raw)
    ser.close()

    if not lines:
        sys.exit("❌ 一帧都没读到。检查：① 板子有电吗 ② 跑车时是否按了 PA12 长按 ③ 是不是已经断电过一次（RAM 会丢）")
    if n_expect is not None and len(lines) != n_expect:
        print(f"      ⚠️ 期望 {n_expect} 帧，实收 {len(lines)} 帧（串口丢了几行，可接受）")

    # ---- 存档 ----
    out_dir = telemetry.new_experiment_dir("snapshot_" + args.note)
    csv_path = out_dir / "telemetry.csv"
    n_t = 0
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=telemetry.CSV_COLUMNS)
        w.writeheader()
        for raw in lines:
            parsed = telemetry.parse_line(raw)
            if not parsed:
                continue
            kind, row = parsed
            row["kind"] = kind          # ⚠️ 必须填：plot.py 靠 kind 列筛出 $T 帧
            if kind == "T":
                n_t += 1
            w.writerow({k: row.get(k, "") for k in telemetry.CSV_COLUMNS})
    print(f"[2/3] 已存档：{out_dir}")
    print(f"      $T {n_t} 帧 → {csv_path.name}")

    with open(out_dir / "note.md", "w", encoding="utf-8") as f:
        f.write(f"# {args.note}\n\n来源：**板载 RAM 快照**（DUMP），不是实时遥测。\n\n"
                f"- 帧数：{n_t}（≈{n_t/100:.1f}s @100Hz）\n"
                f"- 说明：跑车时未接串口；数据由板子记录、事后 DUMP 读出。\n")

    # ---- 出图（复用 plot.py）----
    print("[3/3] 出图…")
    subprocess.run([sys.executable, str(Path(__file__).parent / "plot.py"), str(out_dir)], check=False)
    print(f"\n完成。日志目录：{out_dir}")
    print( "顺手提醒：读完可以发 `python tools/snapshot.py --erase` 或串口 SNAPRESET 清空，准备下一趟。")


if __name__ == "__main__":
    main()
