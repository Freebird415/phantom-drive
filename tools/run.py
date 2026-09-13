"""一键实验：编译烧录 → 采集遥测 → 存档 → 出图（P01）

用法（在仓库根目录执行）：
    python tools/run.py --note "过弯抖" --seconds 30          # 完整流程
    python tools/run.py --note "..." --seconds 30 --no-flash # 只采集（车已在跑）
    python tools/run.py --ports                              # 看有哪些串口

每轮实验存到 logs/<时间>_<描述>/ ：
    telemetry.csv   原始数据        meta.json   代码版本/端口/时长
    note.md         你的描述+摘要    plot.png    曲线图
"""
from __future__ import annotations

import argparse
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
    sys.exit("缺依赖：请先执行  python -m pip install pyserial")


def show_ports():
    ports = list(list_ports.comports())
    if not ports:
        print("没有检测到串口设备。检查 USB-TTL 是否插好、驱动是否安装。")
        return
    print("检测到以下串口：")
    for p in ports:
        print(f"  {p.device:8s}  {p.description}")


def pick_port(explicit: str | None) -> str:
    ports = list(list_ports.comports())
    if explicit:
        return explicit
    if not ports:
        sys.exit("没有检测到串口。检查 USB-TTL 是否插好；用 --ports 查看。")
    # 优先选常见 USB 串口芯片
    for p in ports:
        desc = (p.description or "").lower()
        if any(k in desc for k in ("ch340", "cp210", "usb-serial", "usb serial", "silicon")):
            return p.device
    if len(ports) == 1:
        return ports[0].device
    print("检测到多个串口，无法确定用哪个，请加 --port：")
    show_ports()
    sys.exit(1)


def flash() -> bool:
    print("[1/3] 编译 + 烧录 (ST-Link)…")
    r = subprocess.run([sys.executable, "-m", "platformio", "run", "-d", "firmware", "-t", "upload"],
                       cwd=telemetry.repo_root(), text=True, capture_output=True)
    tail = (r.stdout or "").strip().splitlines()[-6:]
    print("      " + "\n      ".join(tail))
    if r.returncode != 0:
        print("[错误] 烧录失败：检查 ST-Link 接线（SWDIO/SWCLK/GND/3V3）与板子供电。")
        return False
    return True


def main():
    ap = argparse.ArgumentParser(description="一键实验：烧录 + 采集遥测 + 存档")
    ap.add_argument("--note", default="未命名", help="一句话描述本轮做了什么/现象是什么")
    ap.add_argument("--seconds", type=float, default=30, help="采集时长（秒），默认 30")
    ap.add_argument("--port", help="串口号（如 COM5），默认自动选择")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--no-flash", action="store_true", help="跳过编译烧录，直接采集")
    ap.add_argument("--ports", action="store_true", help="列出串口后退出")
    ap.add_argument("--raw", action="store_true", help="只看原始串口输出（不存档不出图，排查接线用）")
    ap.add_argument("--loopback", action="store_true", help="回环自检：短接 USB-TTL 的 TXD-RXD 后运行，验证串口链路")
    args = ap.parse_args()

    if args.loopback:
        port = pick_port(args.port)
        print(f"[loopback] 用一根杜邦线把 USB-TTL 的 TXD 和 RXD 短接起来，然后回车…")
        print(f"[loopback] 端口 {port} @ {args.baud}")
        ser = serial.Serial(port, args.baud, timeout=1)
        hits = 0
        for _ in range(3):
            ser.reset_input_buffer()
            ser.write(b"LOOPBACK-TEST\n")
            time.sleep(0.3)
            got = ser.read(64)
            print(f"   发送 LOOPBACK-TEST → 收到: {got!r}")
            if b"LOOPBACK-TEST" in got:
                hits += 1
        ser.close()
        if hits:
            print("结论：✅ 回环成功——USB-TTL 与驱动正常，问题在小车端（接线或固件）")
        else:
            print("结论：❌ 回环失败——检查短接线是否插紧、TTL 驱动、端口是否被占用")
        return

    if args.ports:
        show_ports()
        return

    if args.raw:
        port = pick_port(args.port)
        print(f"[raw] 打开 {port} @ {args.baud}，{args.seconds:g} 秒（Ctrl+C 提前结束）")
        ser = serial.Serial(port, args.baud, timeout=0.2)
        t0 = time.time()
        n = 0
        try:
            while time.time() - t0 < args.seconds:
                raw = ser.readline()
                if raw:
                    n += 1
                    print(f"  {n:5d} | {raw.decode('utf-8', 'replace').rstrip()}")
        except KeyboardInterrupt:
            pass
        finally:
            ser.close()
        print(f"[raw] 共收到 {n} 行")
        return

    if not args.no_flash and not flash():
        sys.exit(1)

    port = pick_port(args.port)
    out_dir = telemetry.new_experiment_dir(args.note)
    rec = telemetry.Recorder(out_dir, args.note, port, args.baud, args.seconds)

    print(f"[2/3] 采集 {args.seconds:g} 秒（{port} @ {args.baud}）…")
    try:
        ser = serial.Serial(port, args.baud, timeout=0.2)
    except serial.SerialException as e:
        sys.exit(f"[错误] 打不开串口 {port}：{e}\n（串口被占用？关掉 PlatformIO monitor / 其他串口工具）")

    t0 = time.time()
    last_report = t0
    try:
        time.sleep(0.3)
        ser.write(b"GET\n")          # 让固件回一帧 $P —— 保证每份日志都带参数快照
        while time.time() - t0 < args.seconds:
            raw = ser.readline()
            if raw:
                rec.feed(raw.decode("utf-8", "replace"))
            now = time.time()
            if now - last_report >= 5:
                last_report = now
                print(f"      …已收到 {rec.counts['T']} 帧遥测")
    except KeyboardInterrupt:
        print("\n      手动停止")
    finally:
        ser.close()
        rec.close()

    print(f"[3/3] 已存档：{out_dir}")
    print(f"      遥测 {rec.counts['T']} 帧 / 参数 {rec.counts['P']} 帧 / 事件 {rec.counts['E']} 个")
    if rec.counts["T"] == 0:
        print("      ⚠ 一帧都没收到：检查 USB-TTL 接线（TXD↔PA10、RXD↔PA9、GND↔GND）和端口号")

    # 出图
    plot_script = Path(__file__).resolve().parent / "plot.py"
    subprocess.run([sys.executable, str(plot_script), str(out_dir)], check=False)
    print(f"\n完成。日志目录：{out_dir}\n直接把 note.md 与 plot.png 发给智能体即可分析。")


if __name__ == "__main__":
    main()
