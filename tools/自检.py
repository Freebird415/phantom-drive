# -*- coding: utf-8 -*-
"""元件自检 —— 人在车边配合的那几项（保险丝/接线动过之后跑一遍）

用法（仓库根目录，车已烧好固件、USB-TTL 插好）：
    python tools/自检.py                 # 自动找 USB-TTL 的串口
    python tools/自检.py --port COM9     # 指定串口
    python tools/自检.py --skip-motor    # 只测不动车的部分

自动测（不需要人动手）：遥测连通 / 八路灰度 / MPU6050 / 板载快照
需要人配合（脚本会提示）：
    按键     —— 提示后连按几次
    蜂鸣器   —— 响一下，你回答听到没有
    编码器   —— 用手慢转左右轮各 5 圈（脚本读增量，判断两路是否独立）
    电机     —— 【必须先把轮子架空】输入 y 才继续；给 25% 短脉冲，看轮子转不转、朝哪边

测量结果会存到 logs/<时间>_元件自检/ ，留档用。

注意：控制台只输出 GBK 能编码的字符（不用 emoji / 对勾）——Windows 下中文控制台是 GBK。
"""
from __future__ import annotations

import argparse
import io
import sys
import time
from datetime import datetime
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("[!!] 缺 pyserial：先运行  python -m pip install pyserial")
    raise SystemExit(1)

ROOT = Path(__file__).resolve().parents[1]
NOISE = ("$T", "$U")          # 100Hz / 4Hz 的连续帧，过滤掉免得刷屏


class Car:
    def __init__(self, port: str, baud: int = 115200):
        self.ser = serial.Serial(port, baud, timeout=0.2)
        time.sleep(0.3)
        self.ser.reset_input_buffer()

    def close(self):
        self.ser.close()

    def send(self, cmd: str, wait: float = 1.2) -> list[str]:
        """发一条命令，收集回显（自动滤掉 $T/$U 帧）"""
        self.ser.reset_input_buffer()
        self.ser.write((cmd + "\r\n").encode())
        buf = b""
        t0 = time.time()
        while time.time() - t0 < wait:
            buf += self.ser.read(8192)
        out = []
        for line in buf.decode("utf-8", "replace").splitlines():
            line = line.rstrip()
            if line and not line.startswith(NOISE):
                out.append(line)
        return out

    def one(self, cmd: str, wait: float = 1.2) -> str:
        """只取第一条像样的输出。
        注意：各驱动的前缀不一样（`# ` / `OLSCAN> ` / `GR> ` / `HMI> ` / `$E,`），
        所以不能只认 `#`；这里是「第一条含字母的行」——
        顺带滤掉被读串口切断的 `$T/$U` 残片（如 `0,0`、`,0`）。"""
        for line in self.send(cmd, wait):
            if any(ch.isalpha() for ch in line):
                return line
        return "(无响应)"

    def enc(self) -> tuple[int, int]:
        line = self.one("ENC")
        try:
            body = line.split("L=")[1]
            l, r = body.split("R=")
            return int(l.strip()), int(r.split()[0])
        except Exception:
            return 0, 0


def ask(prompt: str, default: str = "") -> str:
    try:
        return input(prompt).strip()
    except (EOFError, KeyboardInterrupt):
        print()
        return default


def pick_port() -> str:
    cands = []
    for p in list_ports.comports():
        desc = (p.description or "") + " " + (p.hwid or "")
        if any(k in desc for k in ("CH340", "USB-SERIAL", "CP210", "FT232", "Silicon")):
            cands.append(p.device)
    if len(cands) == 1:
        return cands[0]
    if not cands:
        print("[!!] 没找到 USB-TTL 串口，请用 --port 指定（先跑 python tools/run.py --ports 看列表）")
        raise SystemExit(2)
    print("[!!] 有多个可选串口：", ", ".join(cands))
    return cands[0]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--skip-motor", action="store_true", help="不测电机（轮子不方便架空时用）")
    args = ap.parse_args()

    port = args.port or pick_port()
    print("=" * 66)
    print("  工创小车 · 元件自检      串口 %s" % port)
    print("=" * 66)
    print("  提示：车要通电（真 5V 更好）；测电机时要先把轮子架空。\n")

    car = Car(port)
    res: list[tuple[str, str, str]] = []      # (项目, 结果, 备注)

    def rec(name, ok, note=""):
        res.append((name, "[OK]" if ok else "[!!]", note))
        print("  %-10s %s %s" % (name, "[OK]" if ok else "[!!]", note))

    # ---------------- 1. 遥测连通 ----------------
    print("[1] 遥测连通")
    line = car.one("PING", 1.0)
    ok = "PONG" in line
    rec("遥测", ok, line if ok else "PING 没回，检查串口/供电")
    if not ok:
        print("\n  连不上就先别往下测了。")
        car.close()
        return 1

    # ---------------- 2. 不需要人动手的项 ----------------
    print("\n[2] I2C 器件 / 快照（自动）")
    g = car.one("GR")
    rec("灰度", "ping=OK" in g, g[:110])
    m = car.one("MPU")
    rec("MPU6050", "0x68" in m, m)
    s = car.one("SNAP?")
    rec("快照", "RAM=" in s, s)

    # ---------------- 3. 按键 ----------------
    print("\n[3] 按键（PA12）")
    print("  请连按几次板上的用户按键（每次按住约半秒）...")
    seen_lo = False
    t0 = time.time()
    while time.time() - t0 < 8:
        if "PA12=0" in car.one("KEY", 0.35):
            seen_lo = True
            break
    rec("按键", seen_lo, "见到按下" if seen_lo else "8 秒内没见到按下（或按键没接好）")
    if not seen_lo:
        print("  参考：正常松开时 KEY 应回 PA12=1")

    # ---------------- 4. 蜂鸣器 ----------------
    print("\n[4] 蜂鸣器")
    car.send("BEEP 300", 1.0)
    ans = ask("  刚才听到蜂鸣器响了吗？(y/n，回车=y) ").lower()
    rec("蜂鸣器", ans in ("", "y", "yes"), "人耳确认")

    # ---------------- 5. 编码器（手转） ----------------
    print("\n[5] 编码器（需要你用手转轮子）")
    print("  先【慢转左轮 5 圈】，转完按回车...")
    ask("  转完左轮按回车（转之前先别动右轮）")
    l0, r0 = car.enc()
    print("  现在【慢转右轮 5 圈】，转完按回车...")
    ask("  转完右轮按回车")
    l1, r1 = car.enc()
    dl, dr = l1 - l0, r1 - r0
    rec("编码器左", abs(dl) > 50, "左轮增量 %+d（右轮 %+d，应接近 0）" % (dl, dr))
    rec("编码器右", abs(dr) > 50, "右轮增量 %+d" % dr)
    if abs(dl) <= 50 and abs(dr) <= 50:
        print("  两路都没动？先看 ENCPIN：四路引脚全钉在一个电平 = 悬空/接线问题")
        print("   ", car.one("ENCPIN"))

    # ---------------- 6. 电机（要点火，必须架空） ----------------
    if args.skip_motor:
        print("\n[6] 电机：已按 --skip-motor 跳过")
    else:
        print("\n[6] 电机（会真的给电）")
        print("  [!!] 确认【轮子已架空、车不会跑】再继续！")
        if ask("  轮子架空了吗？输入 y 继续：").lower() not in ("y", "yes"):
            print("  已跳过电机测试。")
        else:
            for name, cmd, who in (("左轮", "MOT 25 0", "左"), ("右轮", "MOT 0 25", "右")):
                a, b = car.enc()
                car.send(cmd, 0.2)
                time.sleep(1.0)
                car.send("STOP", 0.8)
                c, d = car.enc()
                turned = abs(c - a) > 20 or abs(d - b) > 20
                rec("电机" + who, turned,
                    "%s 给 25%% 一秒 -> 编码器增量 L%+d R%+d" % (name, c - a, d - b))
            print("  请看一眼：两个轮子是朝【前】转的吗？（PWM 正 = 前进，见 决策 E-22）")
            ans = ask("  都朝前转？(y/n) ").lower()
            rec("电机方向", ans in ("", "y", "yes"), "与 MOT_SIGN 宏的约定一致" if ans in ("", "y", "yes")
                else "符号反了：只改 MOT_SIGN_L/R，别改接线")

    car.close()

    # ---------------- 汇总 ----------------
    print("\n" + "=" * 66)
    print("  汇总")
    print("=" * 66)
    bad = [r for r in res if r[1] == "[!!]"]
    for name, flag, note in res:
        print("  %-10s %s %s" % (name, flag, note))
    print("\n  结论：%s" % ("全部通过" if not bad else "%d 项需要处理：%s"
                          % (len(bad), "、".join(r[0] for r in bad))))

    # ---------------- 留档 ----------------
    out = ROOT / "logs" / (datetime.now().strftime("%Y-%m-%d_%H%M") + "_元件自检")
    out.mkdir(parents=True, exist_ok=True)
    with io.open(out / "note.md", "w", encoding="utf-8") as f:
        f.write("# 元件自检\n\n- 时间：%s\n- 串口：%s\n\n" %
                (datetime.now().strftime("%Y-%m-%d %H:%M"), port))
        f.write("| 项目 | 结果 | 备注 |\n|---|---|---|\n")
        for name, flag, note in res:
            f.write("| %s | %s | %s |\n" % (name, flag, note))
        f.write("\n结论：%s\n" % ("全部通过" if not bad else "%d 项需要处理" % len(bad)))
    print("  已存档：logs/%s/note.md" % out.name)
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
