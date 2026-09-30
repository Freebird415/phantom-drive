# -*- coding: utf-8 -*-
"""BLE 日志回传 —— 跑车时不用插串口，无线把板载日志实时收到电脑

用法（仓库根目录执行，车已上电、模块在广播）：
    python tools/btlog.py --scan                       # 先看有哪些 BLE 设备
    python tools/btlog.py --note "A4 直线 kp14" --seconds 30
    python tools/btlog.py --note "..." --seconds 60 --raw      # 顺手存原始字节，排查用
    python tools/btlog.py --name JDY-69A-BLE --seconds 30      # 按名字找（默认）

流程：
    车：串口发 `BTL 1`（开）→ 跑车 → `BTL 0`（关）
    电脑：本脚本连上模块、订阅 FFE1、把 **8 字节二进制帧** 还原成一行 `$T` 文本
          → 复用 tools/telemetry.py 的 Recorder => telemetry.csv / meta.json / note.md
          → 调 plot.py 出图（列与 snapshot.py 完全一致，plot.py 直接能画）

[!] 为什么是二进制帧：BLE 每包只有 20 字节（MTU 23），ASCII 的 `$T` 一行 ~48 字节
   @100Hz = 4.8KB/s 扛不住；8B/帧 @50Hz = 400 B/s 才稳。
   （BTL 默认就是 50Hz，见 firmware/src/app/app_log.h 的 Log_BleTick）
"""
from __future__ import annotations

import argparse
import asyncio
import io
import struct
import subprocess
import sys
from datetime import datetime
from pathlib import Path

try:
    import bleak
    from bleak import BleakClient, BleakScanner
except ImportError:
    print("[!!] 缺 bleak：先运行  python -m pip install bleak")
    raise SystemExit(1)

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import telemetry  # noqa: E402  （复用 CSV/meta/note 与 git 信息）

NOTIFY = "0000ffe1-0000-1000-8000-00805f9b34fb"      # 透传通知特征（各模块都是 FFE1）
# 已知可以用的模块（两边都是 FFE1 透传 + ~60B 写入上限 + ~1 通知/s 的克隆固件）
KNOWN = ["JDY-69A-BLE", "HC-06"]
WRITE = NOTIFY                                        # 同一特征也可写（下行）
SYNC = bytes([0xAA, 0x55, 0xAA, 0x55])   # 固件开流时+每2秒补发的同步头（见 Log_BleTick）
FRAME = 8


class DeFramer:
    """把 BLE 来的字节流切成 8 字节帧，并还原成一行 $T 文本。

    帧布局（见 firmware/src/app/app_log.c 的 LogFrame）：
        bits(1) err10(1) pwm_l(1) pwm_r(1) state(1) dL(1) dR(1) gz8(1)
    """

    def __init__(self):
        self.buf = bytearray()
        self.n = 0
        self.raw = bytearray()
        self.aligned = False      # 先找同步头再开始切帧

    def feed(self, data: bytes) -> list[str]:
        self.raw += data
        self.buf += data
        lines = []

        # ⚠️ 顺序很关键：**对齐状态下绝不能再"先找同步头"** —— 那会把突发中间的帧
        #    全部当垃圾丢掉（本项目踩过两次）。正确顺序：
        #    ① 未对齐 -> 找同步头对齐；② 已对齐 -> 先解帧；③ 解不出才回退重对齐。
        while True:
            if not self.aligned:
                j = self.buf.find(SYNC)
                if j < 0:
                    if len(self.buf) > 64:
                        del self.buf[:-4]        # 留住尾巴，防同步头被切断
                    break
                del self.buf[:j + len(SYNC)]
                self.aligned = True
                continue

            if len(self.buf) >= 4 and bytes(self.buf[:4]) == SYNC:
                del self.buf[:4]                 # 下一个突发的头：跳过，继续解帧
                continue
            if len(self.buf) < FRAME:
                break

            f = bytes(self.buf[:FRAME])
            bits, err10, pl, pr, st, dl, dr, gz = struct.unpack("<BbbbBbbB", f)
            if st not in (0, 1, 4) or abs(err10) > 100:
                self.aligned = False             # 错位：丢 1 字节，回到找同步头
                del self.buf[:1]
                continue
            del self.buf[:FRAME]

            self.n += 1
            gray = ",".join(str((bits >> i) & 1) for i in range(8))
            sign = "-" if err10 < 0 else ""
            e = abs(err10)
            lines.append("$T,%u,%s,%s%d.%d,%d,%d,%d,%d,%d,%d" % (
                self.n * 40,                 # 25Hz => 40ms/帧
                gray, sign, e // 10, e % 10,
                pl, pr, st, dl, dr, gz))
        return lines


async def optimize_link(addr: str) -> bool:
    """向 Windows 请求【高吞吐】连接参数。

    实测（2026-10-01，本项目的两块模块都一样）：
      Windows 默认给 30ms 间隔 + **slave latency 10** ⇒ 模块每 ~330ms 才醒一次；
      请求 ThroughputOptimized **并重连** 后可变成 15ms + latency 0 ✓
    ⚠️ 必须重连才生效（WinRT 的请求只对下一次连接生效）。
    """
    try:
        from winrt.windows.devices.bluetooth import (
            BluetoothLEDevice, BluetoothLEPreferredConnectionParameters)
    except ImportError:
        return False
    try:
        h = await BluetoothLEDevice.from_bluetooth_address_async(int(addr.replace(":", ""), 16))
        h.request_preferred_connection_parameters(
            BluetoothLEPreferredConnectionParameters.throughput_optimized)
        await asyncio.sleep(0.5)
        return True
    except Exception as e:                     # 拿不到就当没有，不影响正常收数据
        print("  （连接参数优化失败：%s）" % e)
        return False


async def scan():
    print("扫描 BLE 设备（8 秒）...")
    devs = await BleakScanner.discover(timeout=8.0, return_adv=True)
    for addr, (d, adv) in devs.items():
        if d.name or adv.local_name:
            print("  %-20s %-18s RSSI=%-4s" % (addr, d.name or adv.local_name, adv.rssi))


async def run(args):
    out_dir = telemetry.new_experiment_dir(args.note or "BLE日志")
    dev_name = [None]
    rec = telemetry.Recorder(out_dir, args.note or "BLE日志", port="BLE:待连接",
                             baud=0, seconds=args.seconds)
    de = DeFramer()
    got = {"n": 0}

    def on_notify(_ch, data: bytes):
        for line in de.feed(bytes(data)):
            rec.feed(line)
            got["n"] += 1

    target = args.addr or args.name or "JDY-69A-BLE"
    if args.addr:
        dev = args.addr
    else:
        # Windows 下必须【先扫描】才能把广播名解析成地址（直接传名字会报 DeviceNotFound）
        cands = [target] if args.name else KNOWN
        dev = None
        for nm in cands:
            print("扫描 BLE 设备，找 %s ..." % nm)
            dev = await BleakScanner.find_device_by_name(nm, timeout=15.0)
            if dev is not None:
                break
        if dev is None:
            print("[!!] 没找到 %s。确认：① 车上总开关打开（模块通电）② 蓝牙适配器已开" % " / ".join(cands))
            print("     可用 --name 指定名字，或先跑 --scan 看有哪些设备")
            return 1
        print("找到：%s (%s)" % (dev.name, dev.address))
        dev_name[0] = dev.name

    print("连接中 ...")
    if args.opt:
        # 先连一次把参数请求塞给 Windows，再重连让参数生效（见 optimize_link 注释）
        # [!] 这一段纯属锦上添花：任何异常都跳过，绝不影响收数据
        c0 = None
        try:
            c0 = BleakClient(dev, timeout=20.0)
            await c0.connect()
            await optimize_link(dev.address)
            await asyncio.sleep(0.3)
            await c0.disconnect()
            await asyncio.sleep(1.0)
        except Exception as e:
            print("  （跳过连接参数优化：%s）" % e)
            try:
                if c0 is not None:
                    await c0.disconnect()
            except Exception:
                pass

    client = BleakClient(dev, timeout=20.0)
    for attempt in range(3):                   # BLE 连接偶发失败：重试几次
        try:
            await client.connect()
            break
        except Exception as e:
            print("  连接失败（第 %d 次）：%s" % (attempt + 1, e))
            if attempt == 2:
                raise
            await asyncio.sleep(1.5)
    mtu = 0
    try:
        mtu = client.mtu_size          # ⚠️ 断开后就取不到了，先存下来
        # Windows 的 BLE 栈偶发"写 CCCD 被取消"（WinError -2147023673）⇒ 重连重试
        for attempt in range(3):
            try:
                await client.start_notify(NOTIFY, on_notify)
                break
            except Exception as e:
                print("  订阅失败（第 %d 次）：%s" % (attempt + 1, e))
                if attempt == 2:
                    raise
                await asyncio.sleep(1.5)
                try:
                    await client.disconnect()
                except Exception:
                    pass
                await asyncio.sleep(1.0)
                await client.connect()
        print("已连接（MTU=%d）→ 订阅 FFE1 完成" % mtu)
        print(">>> 车上只要通电就会回传（BTL 默认开）；要停就发 `BTL 0`")
        t0 = datetime.now()
        while (datetime.now() - t0).total_seconds() < args.seconds:
            await asyncio.sleep(0.5)
            if got["n"] and got["n"] % 100 == 0:
                print("   已收 %d 帧" % got["n"])
        await client.stop_notify(NOTIFY)
    finally:
        await client.disconnect()

    dur = max(0.001, (datetime.now() - t0).total_seconds())
    print("\n收到 %d 帧 / %.1f s => 有效 %.0f B/s（%.1f 帧/s）"
          % (got["n"], dur, len(de.raw) / dur, got["n"] / dur))
    if args.raw:
        (out_dir / "ble_raw.bin").write_bytes(bytes(de.raw))
        print("   原始字节 → ble_raw.bin")

    rec.meta.update({"transport": "BLE", "target": dev_name[0] or target, "mtu": mtu,
                     "frames": got["n"], "bytes": len(de.raw)})
    rec.meta["bytes"] = len(de.raw)
    rec.close()          # meta.json / note.md（传输方式等信息已在 meta 里）
    print("存档：%s" % out_dir)
    subprocess.run([sys.executable, str(HERE / "plot.py"), str(out_dir)], check=False)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scan", action="store_true", help="只扫描附近的 BLE 设备")
    ap.add_argument("--name", default=None, help="按广播名连接（默认自动找 %s）" % "/".join(KNOWN))
    ap.add_argument("--addr", default=None, help="按 MAC 连接")
    ap.add_argument("--note", default="", help="这次跑了什么（进 note.md/目录名）")
    ap.add_argument("--seconds", type=float, default=30.0, help="采集多久（默认 30s）")
    ap.add_argument("--raw", action="store_true", help="同时保存原始字节流")
    ap.add_argument("--opt", action="store_true",
                    help="尝试优化连接参数（实测对本模块无收益，且容易让 Windows BLE 栈不稳；默认关）")
    args = ap.parse_args()
    if args.scan:
        asyncio.run(scan())
        return 0
    asyncio.run(run(args))
    return 0


if __name__ == "__main__":
    sys.exit(main())
