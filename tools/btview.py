# -*- coding: utf-8 -*-
"""BLE 日志实时查看器 —— 自己开、自己看、**按片段导出**（图形界面）

为什么有这个工具（2026-10-01 用户的意见）：
    `btlog.py` 是命令行版：跑起来只有几行中文提示，**看不见数据长什么样**，
    而且必须由人（或智能体）在终端启动 ⇒ 使用者对链路是否真的在收数据没有直观感受。
    本工具把同一条链路做成**自己双击就能开的窗口**：实时滚动显示每一帧、
    显示帧率/累计帧数、自动写 CSV，随时可以自己取走原始数据。

窗口里能看到什么（每收到一帧显示一行）：
    序号  时刻          灰度        err   pwm L/R    st   dL   dR    gz
    1234  12:03:45.123  00011000  +0.5    8/ 8      1    +3   +2    12
    ↑ 序号 = **CSV 里的第几帧**（即使勾了"只看动帧"让某些行不显示，序号也不变）

录制与保存（2026-10-01 用户提的第二个意见：一次全程录制噪音太多）：
    ① 点「开始接收」→ 全量写进 logs/<时间>_<备注>/telemetry.csv（原始的，先不筛）
    ② 想标出"有意思的那一段"：看到关键处按「打点A」/「打点B」（记下帧号+时刻）
    ③ 点「保存日志…」→ 选 [全部] 或 [从第几帧到第几帧] → 导出成一个**独立目录**，
       里面是标准 telemetry.csv（plot.py / 分析脚本直接能用）+ meta.json + note.md
    ④ 导出后**原始日志不动**，可以反复导出多个片段
    ⑤ 关窗口时若还没保存过，会问一句：[保存…] [删除本次] [保留不动] [取消]
       —— 选了"删除"才会真删（防止手一滑丢掉一整趟数据）

用法：
    双击仓库根目录的「蓝牙日志.bat」，或在仓库根目录执行  python tools\\btview.py

数据落盘（与 btlog.py / snapshot.py 同一套格式）：
    logs/<时间>_<备注>/telemetry.csv     ← 本次全部帧
    logs/<时间>_<备注>/markers.txt       ← 打点记录
    logs/<时间>_<备注>_导出NN/telemetry.csv ← 你导出的片段（可直接出图/发人）

[!] 界面上的"只看动帧"只是**显示过滤**，CSV 里始终是全部帧 ✓
"""
from __future__ import annotations

import asyncio
import collections
import csv
import io
import math
import queue
import shutil
import sys
import threading
import time
import tkinter as tk
from datetime import datetime
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

try:
    import bleak  # noqa: F401
    from bleak import BleakClient, BleakScanner
except ImportError:
    tk.Tk().withdraw()
    messagebox.showerror("缺依赖", "缺少 bleak，请先在命令行执行：\n\npython -m pip install bleak")
    raise SystemExit(1)

import telemetry                                     # noqa: E402
import shared_log                                   # noqa: E402  生成“共享日志/”里的小摘要 md
from btlog import KNOWN, NOTIFY, DeFramer            # noqa: E402  复用同一套解帧（格式一致才能复用分析脚本）


def lstrip_num(s: str) -> int:
    """把 '+3' / '-12' / '0' 这类字段安全转成 int。"""
    try:
        return int(s)
    except (TypeError, ValueError):
        return 0


def fmt_frame(idx: int, line: str) -> str:
    """把解帧得到的 `$T,...` 行排成对齐的人类可读一行（含陀螺仪 gz）。"""
    p = line.split(",")
    if len(p) < 17:
        return line
    gray, err, pl, pr, st, dl, dr, gz = (
        "".join(p[2:10]), p[10], p[11], p[12], p[13], p[14], p[15], p[16])
    return "%6d  %-12s  %s  %5s  %4s/%-4s  %2s  %+4s %+4s  %+4s" % (
        idx, datetime.now().strftime("%H:%M:%S.%f")[:-3], gray, err, pl, pr, st, dl, dr, gz)


def frame_fields(line: str):
    """返回 (gray, err, pl, pr, st, dl, dr, gz)；解析不出来就返回 None。"""
    p = line.split(",")
    if len(p) < 17:
        return None
    return ("".join(p[2:10]), p[10], p[11], p[12], p[13], p[14], p[15], p[16])


def _set_readonly(p: Path, on: bool = True):
    """把文件/目录设为（或取消）只读。

    为什么：`logs/` 里的原始数据**只此一份、没有远端备份**（不入库）。
    2026-10-01 踩过：一次 `printf ... > logs/<某次运行>/telemetry.csv` 把真实跑车数据覆盖了 ✗。
    ⇒ 录完就把原始 CSV 设成**只读**：任何误写直接失败；要删必须显式去属性（多一步、但安全）。
    """
    import stat as _stat
    try:
        mode = p.stat().st_mode
        p.chmod(mode & ~_stat.S_IWRITE if on else mode | _stat.S_IWRITE)
    except Exception:
        pass


def _rmtree_force(p: Path):
    """删目录前先取消只读（否则 Windows 上删不掉）。"""
    import shutil as _sh, stat as _stat
    def onerr(func, path, _exc):
        try:
            Path(path).chmod(Path(path).stat().st_mode | _stat.S_IWRITE)
            func(path)
        except Exception:
            pass
    _sh.rmtree(p, onerror=onerr)


def next_clip_dir(session_dir: Path) -> Path:
    """母日志下面的第一个空片段目录：`<母日志>/片段_导出01`、`片段_导出02` …（嵌套式）"""
    for i in range(1, 100):
        d = session_dir / ("片段_导出%02d" % i)
        if not d.exists():
            return d
    return session_dir / "片段_导出99"


def safe_dir_name(s: str) -> str:
    """把用户起的片段名变成合法目录名（去掉路径非法字符）。"""
    name = "".join(c for c in s.strip() if c not in '\\/:*?"<>|')
    return name[:30].strip()


def unique_dir(base: Path) -> Path:
    """base 被占用就加 -2、-3 …"""
    if not base.exists():
        return base
    for i in range(2, 100):
        d = base.with_name(base.name + "-%d" % i)
        if not d.exists():
            return d
    return base


def count_rows(csv_path: Path) -> int:
    """数一个 telemetry.csv 里有多少个 `T` 帧（用来得知某份日志多长）。"""
    try:
        return sum(1 for r in csv.DictReader(io.open(csv_path, encoding="utf-8"))
                   if r.get("kind") == "T")
    except Exception:
        return 0


def dir_size(p: Path, include_children: bool = False) -> int:
    """目录下的字节数（默认不含子目录 —— 子日志单独算，避免重复计）。"""
    total = 0
    try:
        for f in p.rglob("*"):
            if f.is_file():
                if not include_children and f.parent != p:
                    continue
                total += f.stat().st_size
    except Exception:
        pass
    return total


def human_size(n: int) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return "%.0f %s" % (n, unit) if unit == "B" else "%.1f %s" % (n, unit)
        n /= 1024.0
    return "%d B" % n


def scan_logs(logs_root: Path) -> list:
    """递归扫 logs/ 下的日志树（母日志 → 子片段 → 孙片段…），返回顶层列表。

    判定：① `meta.json` 里有 `clip` 字段 ⇒ 片段；② 目录名以 `片段_` 开头或含 `_导出` ⇒ 片段；
    否则当原始录制。层级：谁的目录路径包含了它、且它自己也是一份日志 ⇒ 就是它的子片段。
    """
    def info(d: Path) -> dict:
        meta = {}
        mp = d / "meta.json"
        if mp.exists():
            try:
                import json
                meta = json.loads(mp.read_text(encoding="utf-8"))
            except Exception:
                meta = {}
        is_clip = bool(meta.get("clip")) or d.name.startswith("片段_") or "_导出" in d.name
        return {"path": d, "kind": "clip" if is_clip else "session",
                "name": meta.get("clip", {}).get("name") or d.name,
                "note": meta.get("note") or "",
                "frames": count_rows(d / "telemetry.csv"),
                "size": dir_size(d), "mtime": d.stat().st_mtime, "children": []}

    dirs = ([p for p in logs_root.rglob("*") if p.is_dir() and (p / "telemetry.csv").exists()]
            if logs_root.exists() else [])
    items = {d: info(d) for d in dirs}
    tops = []
    for d in sorted(dirs, key=lambda p: len(p.parts)):      # 从浅到深，保证母日志先生成
        parent, p = None, d.parent
        while p != logs_root.parent and len(p.parts) >= len(logs_root.parts):
            if p in items and p != d:
                parent = p
                break
            if p == logs_root:
                break
            p = p.parent
        if parent is None:
            tops.append(items[d])
        else:
            items[parent]["children"].append(items[d])
    tops.sort(key=lambda x: x["mtime"], reverse=True)
    for t in tops:
        t["children"].sort(key=lambda x: x["mtime"], reverse=True)
    return tops


def promote_children(d: Path, keep_prefix: bool = True) -> list:
    """删除母日志前，把它的子片段**移到上一层**（否则会被一起删掉）。

    提升后的名字带上母日志名（如 `2026-10-01_1500_第二轮_片段_第3个弯道`），
    这样丢掉母目录后仍然看得出它从哪来。
    """
    moved = []
    for ch in list(d.iterdir()):
        if ch.is_dir():
            name = (d.name + "_" + ch.name) if keep_prefix else ch.name
            new = d.parent / name
            try:
                if new.exists():
                    new = unique_dir(new)
                shutil.move(str(ch), str(new))
                moved.append(new)
            except Exception as e:
                print("  提升子片段失败：%s (%s)" % (ch, e))
    return moved


# ==================== 数据分析预处理（每帧派生量）====================
# ⚠️ 尺度常数是**名义值**：mm/计数的精确值仍有争议（见 项目记录/经验与踩坑.md ㊸），
#    所以下面带 mm 的量当“数量级/趋势”用可靠，当绝对尺用要留个心眼。
MM_PER_COUNT = 0.115        # 名义：⌀70mm 轮、1910 计数/转
WB_MM = 150.0               # 两轮中心距（车宽方向）
DT_S = 1.0 / 50.0            # 帧间隔（秒）—— 默认对应固件 BLE 分频 2（50fps）
                            # ⚠️ 运行时会按实收数据自动校正（见 _pump 的 "dt_ms"），
                            #    固件改分频后不需要改这里 ✓（导出片段用的 dt 另从 ms 列推）

# ============ 超声波几何（2026-10-01 用户提供 + 台面实测）============
# 两颗探头**各向外掰**，不是朝前。用户描述：“间距 7cm，左向左偏、右向右偏，
# 两束形成的钝角指向车正前方 110°” —— 等价地：**每颗偏离正前方 35°**
#   （两束之间的夹角 = 180° − 110° = 70° = 35° + 35° ✓ 两种说法同一个形状）
# ⚠️ 这个角度是整个俯视图的基准，若实物改了探头朝向，**必须同步改这里**
#    （确认办法：把书放在车的 35° 斜前方，应只有对应那颗能读到）
US_BASE_MM  = 70.0     # 两颗探头间距（车宽方向）
US_HALF_DEG = 35.0     # 每颗向外掰的角度（偏离正前方）
US_BEAM_DEG = 15.0     # HC-SR04 波束半角（实测 ~15°）
US_MIN_MM   = 50.0     # 实测 <5cm 不可靠（余振）⇒ 图上标个盲区提示
US_RANGE_MM = 1200.0   # 图上显示的最大半径（线性刻度；超过就不画）
US_NEAR_MM  = 300.0    # 与 A02 的 AV_NEAR_CM 一致：进 30cm 就变橙色
# 车体（俯视示意，仅用于画形状；真实尺寸以 CAD 为准）
CAR_LEN_MM  = 200.0    # 车长
CAR_WF_MM   = 180.0    # 车头宽（前驱 ⇒ 前宽后窄）
CAR_WR_MM   = 140.0    # 车尾宽

DERIVED_COLS = ["idx", "t_s", "gray", "err", "pwm_l", "pwm_r", "state", "dl", "dr",
                "mm_l", "mm_r", "speed_mm_s", "yaw_enc_dps", "gyro_dps",
                "turn", "turn_src", "moving"]


def _f(v, d=0.0):
    try:
        return float(v)
    except (TypeError, ValueError):
        return d


def write_derived(src: Path, dst: Path, a: int, b: int) -> dict:
    """把 src 的第 a~b 个 `T` 行写成“已经算好”的 derived.csv，并返回汇总统计。

    为什么要这个：AI（或人）拿到 telemetry.csv 还得自己算速度/转向/里程；
    这里直接把它算好、写成一行一帧的表格 ⇒ 分析更快、也省 token。
    字段定义写在同一目录的 note.md 里（CSV 没法写注释）。
    """
    rows = []
    tidx = 0
    for r in csv.DictReader(io.open(src, encoding="utf-8")):
        if r.get("kind") != "T":
            continue
        tidx += 1
        if tidx > b:
            break
        if tidx >= a:
            rows.append(r)

    st = {"frames": len(rows), "moving": 0, "left": 0, "right": 0, "straight": 0,
          "dist_mm": 0.0, "yaw_enc": 0.0, "yaw_gyro": 0.0, "vmax": 0.0, "vsum": 0.0,
          "dur_s": 0.0, "n_gyro_nonzero": 0}
    # ⚠️ 帧间隔不能写死（固件 BLE 分频可调：分频3 ⇒ 30ms、分频2 ⇒ 20ms；快照 ⇒ 10ms）
    #    2026-10-01 实踩：btlog 曾写死 40ms、又改分频 ⇒ 车速会静默偏 1.5 倍 ✗
    mult, dt_ms = shared_log.frame_timing(rows)        # 两者同源，保证互相一致
    dt = dt_ms / 1000.0
    st["dur_s"] = len(rows) * dt
    with dst.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(DERIVED_COLS)
        for i, r in enumerate(rows, start=a):
            gray = "".join(str(int(_f(r.get("s%d" % k)))) for k in range(8))
            err, pl, pr = _f(r.get("err")), _f(r.get("pwm_l")), _f(r.get("pwm_r"))
            stt = int(_f(r.get("state")))
            dl, dr = _f(r.get("dl")), _f(r.get("dr"))
            gz_dps = _f(r.get("gz"))                    # telemetry 已换算成 °/s
            mm_l, mm_r = dl * MM_PER_COUNT, dr * MM_PER_COUNT
            speed = (mm_l + mm_r) / 2.0 / dt
            # 差速积出来的偏航角速度（正 = 左转/逆时针，与陀螺仪同号）
            yaw_enc = ((mm_r - mm_l) / WB_MM) * 57.2958 / dt if WB_MM else 0.0
            if abs(gz_dps) >= 5.0:
                turn, src_ = ("L" if gz_dps > 0 else "R"), "gyro"
            elif abs(yaw_enc) >= 8.0:
                turn, src_ = ("L" if yaw_enc > 0 else "R"), "enc"
            else:
                turn, src_ = "-", "-"
            moving = 1 if (abs(dl) + abs(dr) > 0 or pl or pr) else 0
            w.writerow([i, "%.3f" % ((i - a) * dt), gray, "%.1f" % err,
                        "%d" % pl, "%d" % pr, stt, "%d" % dl, "%d" % dr,
                        "%.2f" % mm_l, "%.2f" % mm_r, "%.1f" % speed,
                        "%.1f" % yaw_enc, "%.1f" % gz_dps, turn, src_, moving])
            # ⚠️ 每帧的 dl/dr 只是【10ms】的增量，而帧与帧相隔 mult×10ms
            #    ⇒ 按帧累加要 ×mult（BLE = 分频数；快照 = 1）
            #    2026-10-01 实踩：整段“全程”算出 1865mm，乘 3 才对得上真实里程
            st["dist_mm"] += (mm_l + mm_r) / 2.0 * mult
            st["yaw_enc"] += yaw_enc * dt
            st["yaw_gyro"] += gz_dps * dt
            st["vsum"] += speed
            st["vmax"] = max(st["vmax"], abs(speed))
            st["moving"] += moving
            st[{"L": "left", "R": "right"}.get(turn, "straight")] += 1
            if gz_dps:
                st["n_gyro_nonzero"] += 1
    st["vmean"] = st["vsum"] / max(1, st["frames"])
    return st


def session_activity(src: Path) -> tuple:
    """扫一遍日志，找出车子真正“在动/在给电”的第一帧和最后一帧（自动跳掉头尾静止段）。

    用户场景（2026-10-01）：把车放地上后不确定多久才开跑，
    又不想存开头一大堆 0；所以默认就用这个区间，不用自己数帧号。
    """
    first, last, tidx = None, 0, 0
    for r in csv.DictReader(io.open(src, encoding="utf-8")):
        if r.get("kind") != "T":
            continue
        tidx += 1
        active = (abs(_f(r.get("dl"))) + abs(_f(r.get("dr"))) > 0
                  or _f(r.get("pwm_l")) or _f(r.get("pwm_r"))
                  or any(_f(r.get("s%d" % k)) for k in range(8)))
        if active:
            if first is None:
                first = tidx
            last = tidx
    return (first or 1, last or tidx)


def slice_session_csv(src: Path, a: int, b: int, dst: Path) -> int:
    """把 src 里第 a~b 个 `T` 行（含期间的事件行）写成 dst；返回导出的帧数。"""
    rows = list(csv.DictReader(io.open(src, encoding="utf-8")))
    fields = list(rows[0].keys()) if rows else ["kind"] + list(telemetry.CSV_COLUMNS)
    out, tidx, kept = [], 0, 0
    for r in rows:
        if r.get("kind") == "T":
            tidx += 1
            if tidx > b:
                break
            if tidx >= a:
                out.append(r)
                kept += 1
        elif a - 1 <= tidx <= b:            # 事件/参数行：跟着它前面的那几帧一起带走
            out.append(r)
    with dst.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        w.writeheader()
        w.writerows(out)
    return kept


class BleWorker(threading.Thread):
    """在后台线程里跑 asyncio + BLE，把消息投进 queue 给界面。"""

    def __init__(self, q: "queue.Queue", target: str, use_addr: bool,
                 note: str, min_fps: float):
        super().__init__(daemon=True)
        self.q, self.target, self.use_addr = q, target, use_addr
        self.note, self.min_fps = note, min_fps
        self.stop_flag = threading.Event()
        self.rec = None
        self.out_dir = None
        self.n = 0

    # ---- 给界面用的线程安全投递 ----
    def _log(self, text: str):
        self.q.put(("log", text))

    def flush_rec(self):
        """把 CSV 缓冲区刷到磁盘（录制中也能安全导出）。"""
        if self.rec is not None:
            self.rec.flush()

    # ---- 主流程（后台线程）----
    def run(self):
        try:
            asyncio.run(self._main())
        except Exception as e:
            self._log("[!] 出错：%s" % e)
            self.q.put(("done", "出错"))
        else:
            self.q.put(("done", "正常停止"))

    async def _main(self):
        self.out_dir = telemetry.new_experiment_dir(self.note or "BLE日志")
        self.rec = telemetry.Recorder(self.out_dir, self.note or "BLE日志",
                                      port="BLE:待连接", baud=0, seconds=0)
        self.q.put(("session", str(self.out_dir)))
        de = DeFramer()

        def on_notify(_ch, data: bytes):
            for line in de.feed(bytes(data)):
                self.n += 1
                self.rec.feed(line)                     # CSV 永远收全部帧
                self.q.put(("frame", line))             # 排版/过滤都在界面线程做
            self.q.put(("dt_ms", de.frame_ms))          # 帧间隔自动跟随固件分频

        # ---- 找设备 ----
        if self.use_addr:
            dev = self.target
            self._log("[i] 目标（MAC）：%s" % dev)
        else:
            self._log("[i] 扫描 %s ..." % self.target)
            dev = await BleakScanner.find_device_by_name(self.target, timeout=15.0)
            if dev is None:
                self._log("[!] 没找到 %s —— 确认：① 车上总开关打开 ② 电脑蓝牙已开" % self.target)
                self._finish("没找到设备")
                return
            self._log("[i] 找到：%s (%s)" % (dev.name, dev.address))

        # ---- 连接（带重试）----
        client = BleakClient(dev, timeout=20.0)
        mtu = 0
        for attempt in range(3):
            if self.stop_flag.is_set():
                break
            try:
                await client.connect()
                mtu = client.mtu_size
                await client.start_notify(NOTIFY, on_notify)
                break
            except Exception as e:
                self._log("[!] 连接/订阅失败（第 %d 次）：%s" % (attempt + 1, e))
                await asyncio.sleep(1.5)
        else:
            self._log("[!] 三次都没连上，停止")
            self._finish("连接失败")
            return

        self._log("[i] 已连接（MTU=%d），订阅 FFE1 完成" % mtu)
        self.q.put(("stat", {"state": "已连接，试测帧率…"}))

        # ---- 试测几秒，太稀就换会话（这类克隆模块每次连接的速率差别很大）----
        self.n = 0
        await asyncio.sleep(6.0)
        fps = self.n / 6.0
        tries = 0
        while fps < self.min_fps and tries < 3 and not self.stop_flag.is_set():
            tries += 1
            self._log("[!] 本会话太稀（%.1f 帧/s < %.1f）→ 重连换会话（第 %d 次）" % (fps, self.min_fps, tries))
            try:
                await client.stop_notify(NOTIFY)
            except Exception:
                pass
            await client.disconnect()
            await asyncio.sleep(1.5)
            await client.connect()
            await client.start_notify(NOTIFY, on_notify)
            self.n = 0
            await asyncio.sleep(6.0)
            fps = self.n / 6.0
        self._log("[i] 本会话 %.1f 帧/s%s" % (fps, "（够用）" if fps >= self.min_fps else "（偏稀，但继续收）"))
        self.n = 0                                        # 试测的帧不算正式数据

        # ---- 正式采集，直到点停止 ----
        t0 = datetime.now()
        last_n, last_t = 0, t0
        while not self.stop_flag.is_set():
            await asyncio.sleep(0.5)
            now = datetime.now()
            if (now - last_t).total_seconds() >= 3.0:
                cur = self.n / max(0.1, (now - last_t).total_seconds())
                self.q.put(("stat", {"state": "接收中", "fps": cur, "n": self.n,
                                     "sec": (now - t0).total_seconds()}))
                last_n, last_t = self.n, now
            if not getattr(client, "is_connected", True):
                self._log("[!] 连接断开（可能是模块/电脑蓝牙），已停止")
                break

        try:
            await client.stop_notify(NOTIFY)
        except Exception:
            pass
        try:
            await client.disconnect()
        except Exception:
            pass
        self._finish("已停止")

    def _finish(self, why: str):
        if self.rec is None:
            return
        try:
            self.rec.meta.update({"transport": "BLE", "target": self.target,
                                  "frames": self.n, "stopped": why})
            self.rec.close()
        except Exception as e:
            self._log("[!] 写盘出错：%s" % e)
        self._log("[i] 本次共 %d 帧 → %s" % (self.n, self.out_dir))
        self._log("[i] 可用「保存日志…」导出全部/片段；关窗口前不保存的话会问你要不要删")


class ClipDialog(tk.Toplevel):
    """保存对话框：选「全部」或「按帧号范围」导出成独立目录，并可自定义片段名。"""

    def __init__(self, master, session_dir: Path, note: str, n_total: int,
                 keys: list, act: tuple):
        super().__init__(master)
        self.title("保存日志（导出片段）")
        self.resizable(False, False)
        self.transient(master)
        self.grab_set()
        self.result = None                    # (a, b, out_dir)
        self.session_dir, self.note, self.n = session_dir, note, n_total
        self.keys, self.act = list(keys), act

        pad = {"padx": 8, "pady": 4}
        frm = ttk.Frame(self)
        frm.pack(fill="both", expand=True, **pad)

        ttk.Label(frm, text="本次录制：%d 帧" % n_total, font=("", 10, "bold")).grid(
            row=0, column=0, columnspan=4, sticky="w")
        ttk.Label(frm, text="原始文件：%s" % session_dir, foreground="#666").grid(
            row=1, column=0, columnspan=4, sticky="w", pady=(0, 6))

        # ---- 关键帧列表（比赛时一条赛道可能打十几个点）----
        ttk.Label(frm, text="关键帧（按空格/按钮打点）").grid(row=2, column=0, columnspan=2, sticky="w")
        self.lb = tk.Listbox(frm, height=6, width=52, font=("Consolas", 9),
                             activestyle="dotbox", exportselection=False)
        self.lb.grid(row=3, column=0, columnspan=2, sticky="w")
        for k in self.keys:
            self.lb.insert("end", k["text"])
        if not self.keys:
            self.lb.insert("end", "（本次没有打点）")
            self.lb.configure(state="disabled")
        kb = ttk.Frame(frm)
        kb.grid(row=4, column=0, columnspan=2, sticky="w", pady=(2, 6))
        ttk.Button(kb, text="设为起点", width=10, command=lambda: self._use_key("a")).pack(side="left", padx=2)
        ttk.Button(kb, text="设为终点", width=10, command=lambda: self._use_key("b")).pack(side="left", padx=2)

        # ---- 帧号范围 ----
        rng = ttk.Frame(frm)
        rng.grid(row=2, column=2, columnspan=2, sticky="w")
        ttk.Label(rng, text="起始帧").pack(side="left")
        self.v_a = tk.StringVar(value=str(act[0]))
        ttk.Entry(rng, textvariable=self.v_a, width=8).pack(side="left", padx=2)
        ttk.Label(rng, text="结束帧").pack(side="left", padx=(8, 0))
        self.v_b = tk.StringVar(value=str(act[1]))
        ttk.Entry(rng, textvariable=self.v_b, width=8).pack(side="left", padx=2)

        q = ttk.Frame(frm)
        q.grid(row=3, column=2, columnspan=2, sticky="nw")
        ttk.Button(q, text="自动定位有效段", width=14,
                   command=self._auto).pack(anchor="w", pady=1)
        ttk.Button(q, text="全部", width=14, command=self._all).pack(anchor="w", pady=1)
        ttk.Label(q, text="（自动=跳掉头尾静止段\n第一次动第%d帧 ~ 最后动第%d帧）" % (act[0], act[1]),
                  foreground="#0a7", font=("", 8)).pack(anchor="w", pady=(4, 0))

        # ---- 片段名称（给 AI 看的）----
        ttk.Label(frm, text="片段名称").grid(row=5, column=0, sticky="w", pady=(4, 0))
        self.v_name = tk.StringVar(value="")
        e_name = ttk.Entry(frm, textvariable=self.v_name, width=30)
        e_name.grid(row=5, column=1, columnspan=2, sticky="w", pady=(4, 0))
        ttk.Label(frm, text="（例：第3个弯道 / 精停那一下；会进目录名和 note.md）",
                  foreground="#666", font=("", 8)).grid(row=5, column=3, sticky="w", pady=(4, 0))

        # ---- 输出目录 ----
        ttk.Label(frm, text="导出到").grid(row=6, column=0, sticky="w", pady=(6, 0))
        self.v_out = tk.StringVar(value=str(next_clip_dir(session_dir)))
        ttk.Entry(frm, textvariable=self.v_out, width=52).grid(
            row=6, column=1, columnspan=2, sticky="w", pady=(6, 0))
        ttk.Button(frm, text="选择…", command=self._pick).grid(row=6, column=3, sticky="w", pady=(6, 0))

        self.v_info = tk.StringVar()
        ttk.Label(frm, textvariable=self.v_info, foreground="#06c").grid(
            row=7, column=0, columnspan=4, sticky="w", pady=(6, 2))

        bt = ttk.Frame(frm)
        bt.grid(row=8, column=0, columnspan=4, sticky="e", pady=(6, 0))
        ttk.Button(bt, text="导出", command=self._ok).pack(side="right", padx=4)
        ttk.Button(bt, text="取消", command=self.destroy).pack(side="right")

        for v in (self.v_a, self.v_b, self.v_name):
            v.trace_add("write", lambda *_: self._update())
        self._update()
        self.lb.bind("<Double-1>", lambda e: self._use_key("a"))   # 双击 = 设为起点
        e_name.focus_set()

    def _use_key(self, which: str):
        if not self.keys:
            return
        sel = self.lb.curselection()
        if not sel:
            return
        fr = self.keys[sel[0]]["frame"]
        (self.v_a if which == "a" else self.v_b).set(str(fr))

    def _auto(self):
        self.v_a.set(str(self.act[0]))
        self.v_b.set(str(self.act[1]))

    def _all(self):
        self.v_a.set("1")
        self.v_b.set(str(self.n))

    def _pick(self):
        d = filedialog.askdirectory(title="导出到哪个目录（会自动建）")
        if d:
            self.v_out.set(str(Path(d) / Path(self.v_out.get()).name))

    def _range(self):
        try:
            a = max(1, int(self.v_a.get()))
            b = min(self.n, int(self.v_b.get()))
        except (TypeError, ValueError):
            return None
        if a > b:
            return None
        return a, b

    def _update(self):
        r = self._range()
        nm = safe_dir_name(self.v_name.get())
        base = self.session_dir / ("片段_" + nm) if nm else next_clip_dir(self.session_dir)
        self.v_out.set(str(base))
        if r is None:
            self.v_info.set("帧号要填数字，且 起始 ≤ 结束")
        else:
            a, b = r
            extra = ""
            if b - a + 1 >= self.n:
                extra = "   ← 这已经是整份日志，其实可以不用导出（直接用原始日志）"
            elif not nm:
                extra = "   ← 可在「片段名称」里起个名字"
            self.v_info.set("将导出第 %d ~ %d 帧，共 %d 帧（约 %.0f KB）%s"
                            % (a, b, b - a + 1, (b - a + 1) * 0.11, extra))

    def _ok(self):
        r = self._range()
        if r is None:
            messagebox.showwarning("帧号不对", "请检查起始/结束帧号（数字、起始≤结束）", parent=self)
            return
        out = Path(self.v_out.get().strip())
        if not self.v_out.get().strip():
            messagebox.showwarning("没有输出目录", "请填写导出到哪个目录", parent=self)
            return
        self.result = (r[0], r[1], unique_dir(out))
        self.destroy()


class LogBrowser(tk.Toplevel):
    """日志浏览器：树形展示 logs/（母日志 → 子片段），可重开、可清理。"""

    def __init__(self, master, logs_root: Path, current):
        super().__init__(master)
        self.title("打开日志（可继续截取 / 清理）")
        self.geometry("860x460")
        self.transient(master)
        self.logs_root = logs_root
        self.current = Path(current) if current else None
        self.result = None
        self._rows = {}                       # treeview item -> path

        top = ttk.Frame(self)
        top.pack(fill="both", expand=True, padx=8, pady=6)
        cols = ("kind", "frames", "size", "time")
        self.tv = ttk.Treeview(top, columns=cols, show="tree headings", height=14)
        self.tv.heading("#0", text="名称（母日志 → 子片段）")
        self.tv.column("#0", width=420)
        for c, t, w in (("kind", "类型", 70), ("frames", "帧数", 70),
                        ("size", "大小", 80), ("time", "时间", 140)):
            self.tv.heading(c, text=t)
            self.tv.column(c, width=w, anchor="w")
        self.tv.pack(side="left", fill="both", expand=True)
        sb = ttk.Scrollbar(top, orient="vertical", command=self.tv.yview)
        sb.pack(side="right", fill="y")
        self.tv.configure(yscrollcommand=sb.set)
        self.tv.bind("<Double-1>", lambda e: self._open())

        self.v_sum = tk.StringVar()
        ttk.Label(self, textvariable=self.v_sum, foreground="#06c").pack(
            anchor="w", padx=8)
        bt = ttk.Frame(self)
        bt.pack(fill="x", padx=8, pady=6)
        ttk.Button(bt, text="打开为当前日志（可继续截）", command=self._open).pack(side="left", padx=2)
        ttk.Button(bt, text="打开文件夹", command=self._folder).pack(side="left", padx=2)
        ttk.Button(bt, text="删除（保留片段）", command=self._delete).pack(side="left", padx=2)
        ttk.Button(bt, text="刷新", command=self._refresh).pack(side="left", padx=8)
        ttk.Button(bt, text="关闭", command=self.destroy).pack(side="right")
        self._refresh()

    def _refresh(self):
        self.tv.delete(*self.tv.get_children())
        self._rows.clear()
        tops = scan_logs(self.logs_root)
        tot_s = tot_c = n_s = n_c = 0
        for it in tops:
            tot_s += it["size"]
            n_s += 1
            mark = "  ←当前" if self.current and it["path"] == self.current else ""
            node = self.tv.insert("", "end", text="📁 " + it["path"].name + mark,
                                  values=("原始", it["frames"], human_size(it["size"]),
                                          datetime.fromtimestamp(it["mtime"]).strftime("%m-%d %H:%M")))
            self._rows[node] = it["path"]
            for ch in it["children"]:
                tot_c += ch["size"]
                n_c += 1
                mk = "  ←当前" if self.current and ch["path"] == self.current else ""
                cn = self.tv.insert(node, "end", text="└ " + ch["path"].name + mk,
                                    values=("片段", ch["frames"], human_size(ch["size"]),
                                            datetime.fromtimestamp(ch["mtime"]).strftime("%m-%d %H:%M")))
                self._rows[cn] = ch["path"]
            self.tv.item(node, open=True)
        self.v_sum.set("logs/ 共 %s：原始 %d 份（%s）+ 片段 %d 份（%s）"
                       % (human_size(tot_s + tot_c), n_s, human_size(tot_s), n_c, human_size(tot_c)))

    def _sel(self):
        sel = self.tv.selection()
        return self._rows.get(sel[0]) if sel else None

    def _open(self):
        p = self._sel()
        if p is None:
            messagebox.showinfo("先选一个", "在上面的列表里点一下要打开的日志。", parent=self)
            return
        self.result = ("open", str(p))
        self.destroy()

    def _folder(self):
        p = self._sel()
        if p:
            import subprocess
            subprocess.Popen(["explorer", str(p)])

    def _delete(self):
        p = self._sel()
        if p is None:
            messagebox.showinfo("先选一个", "在上面的列表里点一下要删的日志。", parent=self)
            return
        kids = [c for c in p.iterdir() if c.is_dir()]
        msg = ("将删除目录：\n%s\n（%s，含 %d 个子片段）\n\n"
               % (p, human_size(dir_size(p, True)), len(kids)))
        if kids:
            msg += "其中 %d 个子片段会被**移到上一层**保留下来：\n%s\n\n" % (
                len(kids), "\n".join("  · " + k.name for k in kids))
        msg += "删了就不能再从它截取别的区间了。确定删？"
        if not messagebox.askyesno("删除日志", msg, parent=self):
            return
        moved = promote_children(p)
        try:
            moved = promote_children(p)
            _rmtree_force(p)
        except Exception as e:
            messagebox.showerror("删除失败", str(e), parent=self)
            return
        if moved:
            messagebox.showinfo("已删除", "已删除原始日志；\n%d 个片段已提到上一层：\n%s"
                                % (len(moved), "\n".join(m.name for m in moved)), parent=self)
        self._refresh()


class ShareDialog(tk.Toplevel):
    """生成【共享日志】（仓库里的 md 摘要）：填标题/记录者/结论，其余自动生成。"""

    def __init__(self, master, log_dir: Path, default_name: str, n_frames: int):
        super().__init__(master)
        self.title("生成共享日志（进仓库的小摘要）")
        self.resizable(False, False)
        self.transient(master)
        self.grab_set()
        self.result = None
        self.log_dir = log_dir
        f = ttk.Frame(self)
        f.pack(fill="both", expand=True, padx=8, pady=6)
        ttk.Label(f, text="本地完整数据：%s（%d 帧，不会入库）" % (log_dir, n_frames),
                  foreground="#666").grid(row=0, column=0, columnspan=2, sticky="w", pady=(0, 6))
        rows = [("标题", "v_name", default_name),
                ("记录者", "v_who", ""),
                ("固件提交", "v_commit", ""),
                ("一句话备注", "v_note", "")]
        for i, (lab, attr, val) in enumerate(rows, start=1):
            ttk.Label(f, text=lab).grid(row=i, column=0, sticky="w")
            setattr(self, attr, tk.StringVar(value=val))
            ttk.Entry(f, textvariable=getattr(self, attr), width=46).grid(
                row=i, column=1, sticky="w", pady=1)
        ttk.Label(f, text="结论 / 观察" + chr(10) + "（最值钱，别人就靠这段）").grid(
            row=5, column=0, sticky="nw", pady=(6, 0))
        self.txt = tk.Text(f, width=58, height=7, font=("", 9))
        self.txt.grid(row=5, column=1, sticky="w", pady=(6, 0))
        ttk.Label(f, text="会自动带上：帧数/时长/里程/速度/偏差/转角/state/ASCII 曲线/关键帧",
                  foreground="#666", font=("", 8)).grid(row=6, column=0, columnspan=2,
                                                        sticky="w", pady=(4, 0))
        bt = ttk.Frame(f)
        bt.grid(row=7, column=0, columnspan=2, sticky="e", pady=(6, 0))
        ttk.Button(bt, text="生成到 共享日志/", command=self._ok).pack(side="right", padx=4)
        ttk.Button(bt, text="取消", command=self.destroy).pack(side="right")

    def _ok(self):
        if not self.v_name.get().strip():
            messagebox.showwarning("缺标题", "标题不能空（建议 日期_任务号_名字_版本）", parent=self)
            return
        self.result = dict(name=safe_dir_name(self.v_name.get()), who=self.v_who.get().strip(),
                           commit=self.v_commit.get().strip(), note=self.v_note.get().strip(),
                           obs=self.txt.get("1.0", "end").strip())
        self.destroy()


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("工创小车 · 蓝牙日志实时接收")
        self.geometry("1060x660")
        self.q: "queue.Queue" = queue.Queue()
        self.LOGS = HERE.parent / "logs"
        self.worker: BleWorker | None = None
        self.markers: list[str] = []          # 写进 markers.txt
        self.keys: list[dict] = []             # 关键帧（可打很多个）
        self.last_fields = None                # 最近一帧的解析结果（打点时记状态用）
        self.n_recv = 0                       # 收到的帧数 = CSV 第几行（显示的"序号"就是它）
        self.session_dir: Path | None = None
        self.exports: list[tuple] = []        # 已导出的片段
        self.shared_for: str = ""             # 已经生成过共享摘要的日志路径（防空重复 ✓）
        self.recorded: Path | None = None     # **本轮录制**的目录（未保存就提醒用）
        self.nag_ok = False                   # 用户已说过“保留不动”
        self.gyro_hist = collections.deque(maxlen=400)
        self.gyro_sum = 0.0                    # 陀螺仪累计转角（度）
        self.yaw_enc_sum = 0.0                 # 编码器差速累计转角（度）
        self.spd_hist = collections.deque(maxlen=15)   # 最近 ~0.45s 车速，用于平滑
        self.gray_now = "00000000"
        self.err_now = "0.0"
        # ---- 超声波（2026-10-01：$U/$E 也走蓝牙了 ⇒ 这里能实时看两颗探头）----
        self.us_l = None                 # 左探头 mm（None=还没收到；负数=状态码 -1/-2…）
        self.us_r = None
        self.us_t = 0.0                  # 最近一次收到 $U 的时刻
        self.us_n = 0
        self.gyro_last_nonzero = time.time()
        self._gyro_t = datetime.now()
        self._build()
        self.bind("<space>", self._on_space)      # 按空格 = 打一个关键帧
        self.txt.bind("<space>", self._on_space)  # 焦点在数据区时也算（并拦下插入空格）
        self.protocol("WM_DELETE_WINDOW", self.on_close)
        self.after(120, self._pump)

    # ---------- 界面 ----------
    def _build(self):
        self._apply_dark_style()          # ⚠️ 必须在创建 ttk 控件之前（否则滚动条还是浅色 ✗）
        pad = {"padx": 6, "pady": 4}
        top = ttk.Frame(self)
        top.pack(fill="x", **pad)

        ttk.Label(top, text="设备").grid(row=0, column=0, sticky="w")
        self.dev = ttk.Combobox(top, width=20, values=KNOWN)
        self.dev.set(KNOWN[0])
        self.dev.grid(row=0, column=1, sticky="w", padx=4)

        ttk.Label(top, text="备注").grid(row=0, column=2, sticky="w", padx=(10, 0))
        self.note = ttk.Entry(top, width=28)
        self.note.insert(0, "BLE实时")
        self.note.grid(row=0, column=3, sticky="w", padx=4)

        ttk.Separator(top, orient="vertical").grid(row=0, column=4, sticky="ns", padx=6)
        self.btn_start = ttk.Button(top, text="开始接收", command=self.start)
        self.btn_start.grid(row=0, column=5, padx=2)
        self.btn_stop = ttk.Button(top, text="停止", command=self.stop, state="disabled")
        self.btn_stop.grid(row=0, column=6, padx=2)

        ttk.Separator(top, orient="vertical").grid(row=0, column=7, sticky="ns", padx=6)
        self.btn_save = ttk.Button(top, text="保存日志…", command=self.save_dialog, state="disabled")
        self.btn_save.grid(row=0, column=8, padx=2)
        self.btn_open = ttk.Button(top, text="打开日志…", command=self.open_log_dialog)
        self.btn_open.grid(row=0, column=9, padx=2)
        ttk.Button(top, text="打开目录", command=self.open_dir).grid(row=0, column=10, padx=2)
        self.btn_share = ttk.Button(top, text="分享到共享日志…（已自动）", command=lambda: self.share_dialog())
        self.btn_share.grid(row=0, column=11, padx=2)

        opt2 = ttk.Frame(self)
        opt2.pack(fill="x", **pad)
        self.only_move = tk.BooleanVar(value=False)
        ttk.Checkbutton(opt2, text="只看动帧", variable=self.only_move).pack(side="left")
        ttk.Label(opt2, text="（只影响显示；CSV 始终存全部帧，序号 = CSV 行号）",
                  foreground="#777", font=("", 8)).pack(side="left", padx=(2, 10))
        self.autoscroll = tk.BooleanVar(value=True)
        ttk.Checkbutton(opt2, text="自动滚动", variable=self.autoscroll).pack(side="left")
        ttk.Separator(opt2, orient="vertical").pack(side="left", fill="y", padx=8)
        self.btn_key = ttk.Button(opt2, text="打关键帧（空格）", command=self.mark_key)
        self.btn_key.pack(side="left", padx=2)
        ttk.Button(opt2, text="清屏", command=self._clear_view).pack(side="left", padx=6)
        ttk.Button(opt2, text="丢弃本次日志", command=self.discard_session).pack(side="left", padx=2)

        head = "序号  时刻          灰度        err   pwm L/R    st   dL   dR    gz"

        # ---- 底部：状态栏（带状态灯）+ 仪表盘卡片 ----
        bar = tk.Frame(self)
        bar.pack(fill="x", side="bottom")
        self.lamp = tk.Label(bar, text="●", fg="#8a97a3", bg=self.cget("bg"), font=("", 11))
        self.lamp.pack(side="left", padx=(6, 0))
        self.status = tk.StringVar(value="就绪 —— 点「开始接收」；状态栏出现帧率后才算真的在收")
        tk.Label(bar, textvariable=self.status, anchor="w", bg=self.cget("bg")).pack(
            side="left", fill="x", expand=True, padx=(2, 6), pady=2)

        dash = tk.Frame(self, bg="#0d1216")
        dash.pack(fill="x", side="bottom", padx=6, pady=(4, 2))
        self.v_spd, self.s_spd, self.l_spd = self._card(dash, "车速 · 名义尺度")
        self.v_turn, self.s_turn, self.l_turn = self._card(dash, "转向")
        self.v_yaw, self.s_yaw, self.l_yaw = self._card(dash, "角速度 / 累计")
        gf = self._card_frame(dash, "灰度（● = 黑/压线）")
        self.gray_cv = tk.Canvas(gf, width=104, height=19, bg="#141b22", highlightthickness=0, bd=0)
        self.gray_cv.pack(anchor="w", padx=6)
        self.s_gray = tk.StringVar(value="偏差 err = —")
        tk.Label(gf, textvariable=self.s_gray, bg="#141b22", fg="#7f93a6", font=("", 8),
                 anchor="w").pack(anchor="w", padx=6, pady=(1, 4))
        tf_ = self._card_frame(dash, "陀螺仪趋势 · 最近 3.6 秒")
        self.canvas = tk.Canvas(tf_, width=176, height=26, bg="#0d1216", highlightthickness=0, bd=0)
        self.canvas.pack(anchor="w", padx=6)
        self.s_gyro = tk.StringVar(value="")
        self.l_gyro = tk.Label(tf_, textvariable=self.s_gyro, bg="#141b22", fg="#7f93a6",
                               font=("", 8), anchor="w")
        self.l_gyro.pack(anchor="w", padx=6, pady=(1, 4))

        # 超声波卡片：直接看两颗探头“现在测到多少”
        # （2026-10-01 新增：$U 以前只走串口，跑车时插不了线 ✗ ⇒ 现在走蓝牙 ✓）
        uf = self._card_frame(dash, "超声波（左 / 右）")
        self.v_us = tk.StringVar(value="— / —")
        self.l_us = tk.Label(uf, textvariable=self.v_us, bg="#141b22", fg="#5a6b7c",
                             font=("Consolas", 11, "bold"), anchor="w")
        self.l_us.pack(anchor="w", padx=6)
        self.s_us = tk.StringVar(value="等 $U 帧（USB 2Hz）")
        tk.Label(uf, textvariable=self.s_us, bg="#141b22", fg="#7f93a6", font=("", 8),
                 anchor="w").pack(anchor="w", padx=6, pady=(0, 4))

        # ---- 固定表头（独立控件，不随数据滚动消失）----
        tk.Label(self, text=head, bg="#1b2530", fg="#9fb3c8", font=("Consolas", 10),
                 anchor="w", padx=6, pady=3).pack(fill="x", padx=6, pady=(4, 0))

        box = ttk.Frame(self)
        box.pack(fill="both", expand=True, padx=6, pady=(0, 2))
        self.txt = tk.Text(box, wrap="none", bg="#101418", fg="#d0d8e0",
                           insertbackground="#d0d8e0", font=("Consolas", 10),
                           bd=0, highlightthickness=0, padx=6, pady=2)
        self.txt.pack(side="left", fill="both", expand=True)

        # ---- 右侧：超声波俯视图（2026-10-01 新增）----
        # ⚠️ 必须在 scrollbar 之前 pack：否则滚动条会被挤到面板右边去 ✗
        us = tk.Frame(box, bg="#0d1216")
        us.pack(side="right", fill="y", padx=(6, 0))
        tk.Label(us, text="超声波俯视图 · 车头朝上", bg="#0d1216", fg="#6b7c8c",
                 font=("", 8)).pack(anchor="w")
        self.us_W, self.us_H = 236, 252
        self.us_cv = tk.Canvas(us, width=self.us_W, height=self.us_H, bg="#0b1014",
                               highlightthickness=1, highlightbackground="#243040")
        self.us_cv.pack()
        self.v_us_txt = tk.StringVar(value="等 $U 帧 …（点「开始接收」）")
        tk.Label(us, textvariable=self.v_us_txt, bg="#0d1216", fg="#9fb3c8",
                 font=("Consolas", 8), justify="left", anchor="w",
                 wraplength=232).pack(anchor="w", pady=(3, 0))

        sb = ttk.Scrollbar(box, orient="vertical", command=self.txt.yview)
        sb.pack(side="right", fill="y")
        self.txt.configure(yscrollcommand=sb.set)
        for tag, col in (("ev", "#7fb0ff"), ("warn", "#ffb86b"), ("frame", "#d0d8e0"),
                         ("mark", "#8ce99a")):
            self.txt.tag_configure(tag, foreground=col)
        self._dark_titlebar()             # 标题栏/窗口边框改深色（失败就算了，不影响功能）
        self.bind("<Map>", lambda _e: self._dark_titlebar())   # 映射后再补一次（DWM 只对已显示的窗口生效）

    # ---------- 深色窗口（2026-10-01：用户反馈“边框是白的、和深色界面不协调”）----------
    def _apply_dark_style(self):
        """把 ttk 控件全部改深色（默认 Windows ttk 主题忽略颜色，永远是浅灰 ✗）。

        2026-10-01 踩：只改了滚动条，结果顶部两条控制栏还是白的 ✗
        （用户反馈的“白边框”其实指的就是它们 —— 截图里 frm/按钮/下拉框一片浅灰）
        ⇒ 改成：切 clam 主题 + 把 Frame/Label/Button/Entry/Combobox/Checkbutton
           全部配一遍深色；再顺手把下拉框弹出列表（Tk listbox）也设深色 ✓
        """
        try:
            st = ttk.Style(self)
            if "clam" in st.theme_names():
                st.theme_use("clam")
        except Exception:
            return

        BG, FG, PANEL, EDGE, HI = "#0d1216", "#c8d4e0", "#1b2530", "#243040", "#2b3a4a"
        st.configure(".", background=BG, foreground=FG, fieldbackground=PANEL,
                     bordercolor=EDGE, lightcolor=EDGE, darkcolor=EDGE)
        for name in ("TFrame", "TLabelframe", "TLabelframe.Label", "TLabel"):
            st.configure(name, background=BG, foreground=FG)
        st.configure("TLabelframe", bordercolor=EDGE)

        for name in ("TButton", "Toolbutton"):
            st.configure(name, background=PANEL, foreground=FG, bordercolor=EDGE,
                         lightcolor=PANEL, darkcolor=PANEL, relief="flat", padding=(8, 3))
            st.map(name, background=[("disabled", "#141b22"), ("pressed", HI), ("active", "#22303d")],
                   foreground=[("disabled", "#4a5a6a")])

        for name in ("TEntry", "TCombobox", "TSpinbox"):
            st.configure(name, fieldbackground=PANEL, background=PANEL, foreground=FG,
                         bordercolor=EDGE, arrowcolor=FG, lightcolor=PANEL, darkcolor=PANEL,
                         insertcolor=FG)
            st.map(name, fieldbackground=[("readonly", PANEL), ("disabled", "#141b22")],
                   foreground=[("readonly", FG), ("disabled", "#4a5a6a")])

        st.configure("TCheckbutton", background=BG, foreground=FG, indicatorcolor=PANEL,
                     bordercolor=EDGE, lightcolor=PANEL, darkcolor=PANEL)
        st.map("TCheckbutton", background=[("active", BG)],
               indicatorcolor=[("selected", "#4dabf7"), ("pressed", HI)])

        # 下拉框弹出列表是原生 Tk listbox，不走 ttk ⇒ 只能用 option 数据库设色
        for pat, col in (("*TCombobox*Listbox.background", PANEL),
                         ("*TCombobox*Listbox.foreground", FG),
                         ("*TCombobox*Listbox.selectBackground", HI),
                         ("*TCombobox*Listbox.selectForeground", "#ffffff")):
            try:
                self.option_add(pat, col)
            except Exception:
                pass

        for name in ("TScrollbar", "Vertical.TScrollbar", "Horizontal.TScrollbar"):
            st.configure(name, background=PANEL, troughcolor=BG, bordercolor=BG,
                         arrowcolor="#6b7c8c", darkcolor=PANEL, lightcolor=PANEL, relief="flat")
            st.map(name, background=[("active", HI), ("pressed", "#3a4a5a")])

    def _dark_titlebar(self):
        """Windows 10 2004+ / 11：把标题栏与窗口边框也改成深色。

        用 DWM 的 DWMWA_USE_IMMERSIVE_DARK_MODE（新版属性号 20、旧版 Win10 是 19）。
        ⚠️ 必须在窗口**已经映射到屏幕之后**调用才生效 ⇒ 除了 _build 末尾，还挂到 <Map> 上再试一次。
        """
        try:
            import ctypes
            self.update_idletasks()
            hwnd = ctypes.windll.user32.GetParent(self.winfo_id()) or self.winfo_id()
            for attr in (20, 19):
                ctypes.windll.dwmapi.DwmSetWindowAttribute(
                    hwnd, attr, ctypes.byref(ctypes.c_int(1)), ctypes.sizeof(ctypes.c_int))
        except Exception:
            pass

    # ---------- 仪表盘小卡片 ----------
    def _card_frame(self, parent, title):
        f = tk.Frame(parent, bg="#141b22", highlightbackground="#243040", highlightthickness=1)
        f.pack(side="left", padx=4, pady=2, fill="y")
        tk.Label(f, text=title, bg="#141b22", fg="#6b7c8c", font=("", 8),
                 anchor="w").pack(anchor="w", padx=6, pady=(3, 0))
        return f

    def _card(self, parent, title):
        f = self._card_frame(parent, title)
        v = tk.StringVar(value="—")
        lbl = tk.Label(f, textvariable=v, bg="#141b22", fg="#d0d8e0",
                       font=("Consolas", 12, "bold"), anchor="w")
        lbl.pack(anchor="w", padx=6)
        sub = tk.StringVar(value="")
        tk.Label(f, textvariable=sub, bg="#141b22", fg="#7f93a6", font=("", 8),
                 anchor="w").pack(anchor="w", padx=6, pady=(0, 4))
        return v, sub, lbl

    # ---------- 动作 ----------
    def start(self):
        if self.worker and self.worker.is_alive():
            return
        if self.n_recv and self.recorded and not self.nag_ok and self.session_dir == self.recorded:
            if not self._ask_unsaved("要开始新的一次录制"):
                return
        self.n_recv, self.keys, self.markers = 0, [], []
        self.last_fields = None
        self.recorded, self.nag_ok = None, False
        self.exports, self.session_dir = [], None
        self.gyro_hist.clear()
        self.gyro_sum = 0.0
        self.worker = BleWorker(self.q, self.dev.get().strip(),
                                use_addr=":" in self.dev.get(),
                                note=self.note.get().strip(),
                                min_fps=3.0)
        self.worker.start()
        self.btn_start.configure(state="disabled")
        self.btn_stop.configure(state="normal")
        self.status.set("正在连接…（连上后会自动试测 6 秒帧率）")

    def stop(self):
        if self.worker:
            self.worker.stop_flag.set()
            self.status.set("正在停止并写盘…")

    def _clear_view(self):
        self.txt.delete("1.0", "end")
        self._append("[i] 只是清屏，日志文件没动（要删请用「丢弃本次日志」）")

    def _on_space(self, _e=None):
        self.mark_key()
        return "break"                           # 不让空格插进文本区

    def mark_key(self):
        """按一下 = 记一个关键帧（比赛时一条赛道可能打十几个）。"""
        if not self.n_recv:
            return
        t = datetime.now().strftime("%H:%M:%S")
        gz = self.gyro_hist[-1] * 2.0 if self.gyro_hist else 0.0
        how = "静止" if abs(gz) < 3 else ("左转" if gz > 0 else "右转")
        d = ""
        if self.last_fields:
            gray, err = self.last_fields[0], self.last_fields[1]
            d = "  灰度%s err=%s" % (gray, err)
        text = "#%d 第 %d 帧 %s  gz=%+.0f°/s %s%s" % (
            len(self.keys) + 1, self.n_recv, t, gz, how, d)
        self.keys.append({"n": len(self.keys) + 1, "frame": self.n_recv,
                          "time": t, "gz": gz, "turn": how, "text": text})
        self.markers.append(text)
        self._append("[关键帧] " + text, "mark")
        self.btn_key.configure(text="打关键帧（空格）· 已打 %d 个" % len(self.keys))

    def open_dir(self):
        d = self.session_dir
        if d and Path(d).exists():
            import subprocess
            subprocess.Popen(["explorer", str(d)])
        else:
            messagebox.showinfo("还没有日志", "先点「开始接收」，日志目录会在那时创建。")

    # ---------- 保存 / 删除 ----------
    def open_log_dialog(self):
        """打开历史的原始日志或片段，把它作为“当前日志”继续截取。"""
        d = LogBrowser(self, self.LOGS, self.session_dir)
        self.wait_window(d)
        if d.result and d.result[0] == "open":
            self._open_log_as_current(Path(d.result[1]))

    def _open_log_as_current(self, path: Path):
        n = count_rows(path / "telemetry.csv")
        if not n:
            messagebox.showerror("打不开", "%s\n里没有可用的 telemetry.csv（或没有数据帧）。" % path)
            return
        self.session_dir = path
        self.n_recv = n
        self.exports, self.keys, self.markers, self.recorded = [], [], [], None
        self.nag_ok = True                     # 打开旧日志不算“本轮录制”，不再唠叨
        self.btn_save.configure(state="normal")
        self.btn_key.configure(text="打关键帧（空格）")
        mk = path / "markers.txt"
        if mk.exists():                        # 尽量把当年的关键帧读回来
            import re
            for line in mk.read_text(encoding="utf-8").splitlines():
                m = re.search(r"第\s*(\d+)\s*帧", line)
                if m:
                    self.keys.append({"n": len(self.keys) + 1, "frame": int(m.group(1)),
                                      "text": line, "time": "", "gz": 0.0, "turn": ""})
            if self.keys:
                self.btn_key.configure(text="打关键帧（空格）· 旧日志里有 %d 个" % len(self.keys))
        self._append("[i] 已打开为当前日志：%s（%d 帧%s）⇒ 可用「保存日志…」从它继续截取"
                     % (path.name, n, "，含 %d 个关键帧" % len(self.keys) if self.keys else ""), "mark")
        self.status.set("当前日志：%s（%d 帧）—— 点「保存日志…」截取子片段" % (path.name, n))

    def save_dialog(self):
        if not (self.worker and self.session_dir and self.session_dir.exists()):
            messagebox.showinfo("还没有日志", "先点「开始接收」录一段，再来保存。")
            return None
        try:
            self.worker.flush_rec()          # 录制中也要保证磁盘上有完整数据
        except Exception:
            pass
        try:                                 # 自动定位“车子真正在动”的第一/最后一帧
            act = session_activity(self.session_dir / "telemetry.csv")
        except Exception:
            act = (1, max(1, self.n_recv))
        d = ClipDialog(self, self.session_dir, self.note.get().strip(),
                       self.n_recv, self.keys, act)
        self.wait_window(d)
        if d.result is None:
            return None
        a, b, out = d.result
        if (a, b) == (1, self.n_recv):
            if not messagebox.askyesno(
                    "不用导出副本",
                    "你选的是**整份日志**（第 1 ~ %d 帧），导出来只是复制一份。\n\n"
                    "直接用这份就行：\n%s\n\n仍然要另存一份吗？" % (self.n_recv, self.session_dir)):
                self._append("[i] 整份日志，未重复导出；直接用 %s" % self.session_dir, "mark")
                return None
        try:
            out.mkdir(parents=True, exist_ok=True)
            src = self.session_dir / "telemetry.csv"
            n = slice_session_csv(src, a, b, out / "telemetry.csv")
            st = write_derived(src, out / "derived.csv", a, b)
            (out / "meta.json").write_text(
                '{\n  "note": %s,\n  "kind": "clip",\n  "name": %s,\n  "transport": "BLE",\n'
                '  "clip": {"from": %d, "to": %d, "frames": %d, "src": %s},\n'
                '  "chain": %s,\n  "saved": "%s"\n}\n'
                % (_json_str(self.note.get().strip()),
                   _json_str(safe_dir_name(d.v_name.get())),
                   a, b, n, _json_str(str(self.session_dir)),
                   _json_str(self._chain(out)),
                   datetime.now().isoformat(timespec="seconds")),
                encoding="utf-8")
            self._write_clip_note(out, a, b, n, st, d.v_name.get().strip())
        except Exception as e:
            messagebox.showerror("导出失败", "写文件出错：\n%s" % e)
            return None
        self.exports.append((a, b, str(out)))
        # ⭐ 导出片段也设只读（原始 CSV 不留可写的副本）
        for f in out.rglob("*"):
            if f.is_file():
                _set_readonly(f, True)
        if self.session_dir == self.recorded:
            self.nag_ok = True                            # 本轮已保存过，不再唠叨
        self.auto_share(out, d.result["name"] or out.name, d.result.get("who", ""),
                        note_extra="由导出片段自动生成")     # ⭐ 默认开共享
        self._append("[保存] 第 %d~%d 帧（共 %d 帧）→ %s" % (a, b, n, out), "mark")
        self.status.set("已导出：%s" % out)
        if messagebox.askyesno("导出完成",
                               "已导出第 %d~%d 帧（共 %d 帧）\n\n%s\n\n"
                               "现在打开这个目录吗？" % (a, b, n, out)):
            import subprocess
            subprocess.Popen(["explorer", str(out)])
        return out

    def share_dialog(self, name_hint: str = ""):
        """把当前日志/片段生成一份小的 md 摘要，写进仓库的 共享日志/（可入库）"""
        if not (self.session_dir and (self.session_dir / "telemetry.csv").exists()):
            messagebox.showinfo("还没有日志", "先录一段或打开一份日志，再来分享。")
            return None
        hint = name_hint or ("%s_%s" % (datetime.now().strftime("%Y-%m-%d"),
                                        self.note.get().strip() or "日志"))
        d = ShareDialog(self, self.session_dir, hint, self.n_recv)
        self.wait_window(d)
        if d.result is None:
            return None
        try:
            rows = shared_log.load(self.session_dir)
            md = shared_log.build_md(self.session_dir, d.result["name"], d.result["who"],
                                     d.result["commit"], d.result["note"], d.result["obs"], rows)
            out = shared_log.SHARED / ("%s.md" % d.result["name"])
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(md, encoding="utf-8")
        except Exception as e:
            messagebox.showerror("生成失败", str(e))
            return None
        self._append("[共享] 已生成 %s（%.1f KB）—— 记得补“结论”并提交" %
                     (out.relative_to(shared_log.REPO), out.stat().st_size / 1024.0), "mark")
        if messagebox.askyesno("共享版已生成",
                               "已写入：" + chr(10) + "%s" % out.relative_to(shared_log.REPO)
                               + chr(10) * 2 + "（本地 CSV 全量仍只在 logs/，不入库）"
                               + chr(10) * 2 + "现在打开看看吗？"):
            import subprocess
            subprocess.Popen(["notepad", str(out)])
        return out

    def auto_share(self, log_dir: Path, name: str, who: str = "", note_extra: str = ""):
        """自动生成一份可入库的摘要（默认开共享）——失败不影响主流程。"""
        try:
            rows = shared_log.load(log_dir)
            if not rows:
                return None
            md = shared_log.build_md(log_dir, name, who, "", note_extra, "", rows)
            out = shared_log.SHARED / ("%s%s.md" % (shared_log.PREFIX, name))
            # ⚠️ 2026-10-01 实踩：同名会**直接覆盖** —— 两次跑车备注都叫 “BLE实时” 时，
            #    后一趟把前一趟的摘要（连带手动写的“结论”）盖掉了 ✗ ⇒ 同名自动加序号 ✓
            if out.exists():
                for i in range(2, 100):
                    cand = shared_log.SHARED / ("%s%s-%d.md" % (shared_log.PREFIX, name, i))
                    if not cand.exists():
                        out = cand
                        break
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(md, encoding="utf-8")
            self._append("[共享] 已自动生成 %s（%.1f KB）—— 补上「结论」后即可提交"
                         % (out.relative_to(shared_log.REPO), out.stat().st_size / 1024.0), "mark")
            return out
        except Exception as e:
            self._append("[!] 自动生成共享版失败（不影响记录）：%s" % e)
            return None
        self.shared_for = str(log_dir)

    def _chain(self, out: Path) -> list:
        """记录“这份片段是从哪一路截出来的”，便于日后追溯多层嵌套。"""
        chain, d = [], self.session_dir
        while d and str(d).startswith(str(self.LOGS)):
            chain.append(d.name)
            if d == self.LOGS:
                break
            d = d.parent
        return list(reversed(chain)) + [out.name]

    def _write_clip_note(self, out: Path, a: int, b: int, n: int, st: dict, name: str):
        keys_in = [k for k in self.keys if a <= k["frame"] <= b]
        lines = [
            "# 日志片段 · %s" % (name or (self.note.get().strip() or "未命名")),
            "",
            "- 片段名称：**%s**" % (name or "（未命名）"),
            "- 来源：`%s`" % self.session_dir,
            "- 上级日志：%s" % (self.session_dir.name if self.session_dir else "（无）"),
            "- 范围：第 **%d ~ %d** 帧（本次录制共 %d 帧）" % (a, b, self.n_recv),
            "- 片段帧数：%d，名义时长：%.1f s（BLE 每帧名义 30ms）" % (n, st.get("dur_s", 0)),
            "- 导出时间：%s" % datetime.now().isoformat(timespec="seconds"),
            "",
            "## 一、已经算好的汇总（不用再自己算）",
            "",
            "| 指标 | 值 |",
            "|---|---|",
            "| 帧数 / 其中“在动或给电” | %d / %d |" % (st.get("frames", 0), st.get("moving", 0)),
            "| 名义时长 | %.1f s（每帧 30ms）|" % st.get("dur_s", 0),
            "| 里程（差速推算，名义值） | %.0f mm |" % st.get("dist_mm", 0.0),
            "| 平均速度 / 最大 | %.1f / %.1f mm/s |" % (st.get("vmean", 0.0), st.get("vmax", 0.0)),
            "| 左转 / 右转 / 直行帧数 | %d / %d / %d |" % (st.get("left", 0), st.get("right", 0), st.get("straight", 0)),
            "| 累计转角·陀螺仪 | %+.1f° |" % st.get("yaw_gyro", 0.0),
            "| 累计转角·编码器差速 | %+.1f° |" % st.get("yaw_enc", 0.0),
            "| gz 非零帧（陀螺仪有没有在报数）| %d |" % st.get("n_gyro_nonzero", 0),
            "",
            "> 两个“累计转角”对不上时，常见原因：轮子打滑（编码器多）/ 陀螺仪零偏漂移（陀螺仪多）。",
            "> mm、mm/s 用的是**名义尺度** `%.3f mm/计数`（精确值仍有争议，见 项目记录/经验与踩坑.md ㊸）——" % MM_PER_COUNT,
            "> 拿来比趋势/形状可靠，当绝对尺要小心。",
            "",
            "## 二、文件说明",
            "",
            "- `telemetry.csv`：原始逐帧遥测（与其它脚本同一格式，`plot.py` 直接能画）",
            "- `derived.csv`：**已算好派生量**的逐帧表（列义见下），序号 `idx` 与 `telemetry.csv` 的序号一一对应",
            "",
            "| derived.csv 列 | 含义 |",
            "|---|---|",
            "| idx / t_s | 帧号 / 名义时间（秒，从片段开头算）|",
            "| （每行 = 一个 BLE 帧 = **10ms** 的增量；要按行累加请 ×3）|",
            "| gray | 八路灰度，1=黑（压线），按第1~8路顺序 |",
            "| err | 循迹偏差（−8~+8，正=线偏右）|",
            "| pwm_l / pwm_r | 左右轮 PWM 命令（−100~100）|",
            "| state | 0=丢线停车 1=巡线 4=精停 |",
            "| dl / dr | 左右轮编码器增量（计数，已翻正）|",
            "| mm_l / mm_r | 增量换算成 mm（名义尺度）|",
            "| speed_mm_s | 车身速度 = (mm_l+mm_r)/2 ÷ 30ms |",
            "| yaw_enc_dps | 差速推算的偏航角速度（°/s，正=左转）|",
            "| gyro_dps | 陀螺仪偏航角速度（°/s，正=左转）|",
            "| turn / turn_src | 转向判定 L/R/- 及其来源（gyro=陀螺仪优先，enc=退而用编码器）|",
            "| moving | 1 = 轮子在动 或 电机有命令 |",
            "",
            "## 三、关键帧（打点）",
            "",
        ]
        lines += ["- " + k["text"] for k in keys_in] if keys_in else ["- （本片段范围内没有关键帧）"]
        allk = self.keys
        if allk:
            lines += ["", "（本次录制全部关键帧：%d 个）" % len(allk)]
        lines += [
            "",
            "## 四、这个片段对应什么",
            "（写一句：比如“第 3 个弯道，车在这里右转后回线”）",
            "",
        ]
        (out / "note.md").write_text("\n".join(lines), encoding="utf-8")

    def discard_session(self):
        if self.recorded is None or self.session_dir != self.recorded:
            messagebox.showinfo("不是本轮录制",
                                "当前显示的是打开的历史日志，不能在这里删。\n"
                                "请用「打开日志…」里的「删除（保留片段）」。")
            return
        if not (self.session_dir and self.session_dir.exists()):
            messagebox.showinfo("没有日志", "这一轮还没有录制日志。")
            return
        n = self.n_recv
        kept = "\n".join("  · %s" % e[2] for e in self.exports) or "  （本次还没有导出过片段）"
        if not messagebox.askyesno("删除本次原始日志",
                                   "将【删除】原始日志目录：\n%s\n（共 %d 帧、打点 %d 个）\n\n"
                                   "已经导出的片段【不受影响】：\n%s\n\n"
                                   "删了就不能再从它导别的区间了。确定删？"
                                   % (self.session_dir, n, len(self.keys), kept)):
            return
        try:
            moved = promote_children(self.session_dir)   # 先保住子片段，再删母目录
            _rmtree_force(self.session_dir)              # 只读文件要先去属性
            self._append("[i] 已删除 %s%s" % (self.session_dir,
                                          "；%d 个片段已提到上一层" % len(moved) if moved else ""), "warn")
        except Exception as e:
            messagebox.showerror("删除失败", str(e))
            return
        self.n_recv = 0
        self.session_dir = None
        self.recorded = None
        self.exports = []
        self.keys, self.markers = [], []
        self.btn_key.configure(text="打关键帧（空格）")
        self.btn_save.configure(state="disabled")

    def _ask_unsaved(self, what: str) -> bool:
        """有未保存的**本轮录制**时问一句。返回 True = 可以继续（已保存/已删/保留）"""
        if self.recorded is None or self.session_dir != self.recorded:
            return True
        if not (self.session_dir and self.session_dir.exists() and self.n_recv):
            return True
        ans = messagebox.askyesnocancel(
            "还没保存",
            "%s\n\n本轮录制了 %d 帧（%s），还没保存。\n\n"
            "是 = 先保存片段（也可以稍后在「打开日志…」里重开再截）\n"
            "否 = 删除本次日志\n取消 = 保留不动（之后可在「打开日志…」里找到）"
            % (what, self.n_recv, self.session_dir.name))
        if ans is True:                       # 先保存
            self.save_dialog()
            return True
        if ans is False:                      # 删除
            self.discard_session()
            return True
        self.nag_ok = True                    # 取消 = 保留，不再唠叨
        return True

    def on_close(self):
        # ⭐ 默认开共享：若这趟还没生成过摘要，关窗前补一份（否则“导出忘了点”就没有留档 ✗）
        try:
            if (self.session_dir and self.n_recv
                    and str(self.session_dir) != self.shared_for):
                self.auto_share(self.session_dir,
                                "%s_%s" % (datetime.now().strftime("%Y-%m-%d"),
                                            self.note.get().strip() or "日志"),
                                note_extra="自动生成（关窗时）")
        except Exception:
            pass
        if self.worker and self.worker.is_alive():
            self.worker.stop_flag.set()
            time.sleep(0.4)
        if (self.recorded and self.session_dir == self.recorded and not self.nag_ok
                and self.session_dir.exists() and self.n_recv):
            ans = messagebox.askyesnocancel(
                "关闭前",
                "本轮录制了 %d 帧，还没保存。\n\n"
                "是 = 先保存片段\n否 = 删除本次日志\n取消 = 保留日志并关闭\n\n"
                "（保留的话，下次用「打开日志…」能找到它再截）" % self.n_recv)
            if ans is True:
                if self.save_dialog() is None:
                    return                     # 没导出成功就别关，回去重来
            elif ans is False:
                try:
                    promote_children(self.session_dir)
                    _rmtree_force(self.session_dir)
                    self._append("[i] 已删除 %s" % self.session_dir, "warn")
                except Exception:
                    pass
        self.destroy()

    # ---------- 界面刷新 ----------
    def _append(self, text: str, tag: str = "ev"):
        self.txt.insert("end", text + "\n", tag)
        if self.autoscroll.get():
            self.txt.see("end")

    def _pump(self):
        try:
            while True:
                kind, payload = self.q.get_nowait()
                if kind == "log":
                    self._append(payload, "warn" if payload.startswith("[!]") else "ev")
                elif kind == "session":
                    self.session_dir = Path(payload)
                    self.recorded = self.session_dir          # 本轮录制的母日志
                    self.nag_ok = False
                    self.btn_save.configure(state="normal")
                elif kind == "dt_ms":
                    global DT_S
                    if payload and abs(payload - DT_S * 1000.0) > 0.5:
                        DT_S = payload / 1000.0             # 仪表盘立刻改用它，不再等改代码
                elif kind == "frame":
                    # ---- 超声波（$U）与事件（$E）：2026-10-01 起也走蓝牙 ----
                    if payload.startswith("$U,"):
                        p = payload.split(",")
                        if len(p) >= 4:
                            try:
                                nl, nr = int(p[2]), int(p[3])
                            except ValueError:
                                pass
                            else:
                                # -6（触发太快被驱动拦下）/ -3（上一发没完）不是“没测到” ✓
                                # 它们的意思是“本轮没测，上一个值仍有效” ⇒ 保留旧值，别让圆弧闪掉
                                if nl not in (-6, -3):
                                    self.us_l = nl
                                if nr not in (-6, -3):
                                    self.us_r = nr
                                self.us_t = time.time()
                                self.us_n += 1
                        continue                         # 别打到日志区（2Hz 会很吵 ✗）
                    if payload.startswith("$E,"):
                        self._append(payload, "ev")      # 事件很少，值得占行（和串口一致 ✓）
                        continue
                    f = frame_fields(payload)
                    if f is None:
                        continue
                    gray, err, pl, pr, st, dl, dr, gz = f
                    self.last_fields = f                       # 打关键帧时记状态用
                    self.n_recv += 1                          # 序号 = CSV 第几帧（与显示过滤无关）
                    show = True
                    if self.only_move.get():
                        show = not (gray.strip("0") == ""
                                    and lstrip_num(dl) == 0 and lstrip_num(dr) == 0
                                    and lstrip_num(pl) == 0 and lstrip_num(pr) == 0)
                    if show:
                        self._append(fmt_frame(self.n_recv, payload), "frame")
                    # ---- 仪表盘累加量（gz 单位 = 2°/s 每格；每帧名义 30ms）----
                    g = lstrip_num(gz)
                    self.gyro_hist.append(g)
                    self.gyro_sum += g * 2.0 * DT_S
                    if g:
                        self.gyro_last_nonzero = time.time()
                    dl_n, dr_n = lstrip_num(dl), lstrip_num(dr)
                    self.spd_hist.append((dl_n + dr_n) / 2.0 * MM_PER_COUNT / DT_S)
                    self.yaw_enc_sum += ((dr_n - dl_n) * MM_PER_COUNT / WB_MM) * 57.2958
                    self.gray_now, self.err_now = gray, err
                elif kind == "stat":
                    s = payload
                    state = s.get("state", "")
                    self.lamp.configure(fg="#8ce99a" if "接收中" in state else "#ffd43b")
                    self.status.set("状态：%s   本次 %d 帧（已导出 %d 段）   %.1f 帧/s   已运行 %.0f s"
                                    % (s.get("state", ""), self.n_recv, len(self.exports),
                                       s.get("fps", 0.0), s.get("sec", 0.0)))
                elif kind == "done":
                    self.btn_start.configure(state="normal")
                    self.btn_stop.configure(state="disabled")
                    self.status.set("已停止（%s）—— 点「保存日志…」导出全部或片段" % payload)
                    if self.markers and self.session_dir:
                        try:
                            (self.session_dir / "markers.txt").write_text(
                                "\n".join(self.markers), encoding="utf-8")
                            self._append("[i] 打点已写入 markers.txt", "mark")
                        except Exception:
                            pass
                    # ⭐ 机械防护：录完把原始 CSV 设成只读（防误写/误覆盖）
                    if self.session_dir:
                        for f in ("telemetry.csv", "meta.json", "note.md", "markers.txt"):
                            f2 = self.session_dir / f
                            if f2.exists():
                                _set_readonly(f2, True)
                        self._append("[i] 原始数据已设为只读（防误写；要删请用「丢弃本次日志」）", "ev")
                    # ⭐ 默认开共享：录完自动生成一份可入库的小摘要（3~6KB）
                    if self.session_dir and self.n_recv:
                        self.auto_share(self.session_dir,
                                        "%s_%s" % (datetime.now().strftime("%Y-%m-%d"),
                                                    self.note.get().strip() or "日志"),
                                        who="", note_extra="自动生成（录制结束时）")
        except queue.Empty:
            pass
        now = datetime.now()
        if (now - self._gyro_t).total_seconds() >= 0.1:      # 仪表盘每秒刷 10 次（超声波/灰度跟着变快）
            self._gyro_t = now
            self._refresh_dash()
        self.after(80, self._pump)

    def _yaw_enc_dps(self) -> float:
        """用最近一帧的左右轮增量算偏航角速度（°/s，正 = 左转）—— 与 derived.csv 同一套公式。"""
        if not self.last_fields:
            return 0.0
        dl, dr = lstrip_num(self.last_fields[5]), lstrip_num(self.last_fields[6])
        return ((dr - dl) * MM_PER_COUNT / WB_MM) * 57.2958 / DT_S

    def _refresh_dash(self):
        # ---------- 车速 ----------
        spd = (sum(self.spd_hist) / len(self.spd_hist)) if self.spd_hist else 0.0
        moving = abs(spd) > 3.0
        self.v_spd.set("%+6.1f mm/s" % spd if moving else "   0.0 mm/s")
        self.l_spd.configure(fg="#8ce99a" if moving else "#7f93a6")
        self.s_spd.set(("≈ %.1f cm/s" % (spd / 10.0)) if moving else "静止")

        # ---------- 转向（陀螺仪优先，没数据就看编码器差速）----------
        gz = self.gyro_hist[-1] * 2.0 if self.gyro_hist else 0.0
        yaw_enc = self._yaw_enc_dps()
        if abs(gz) >= 3.0:
            turn, src, rate = ("左转", "陀螺仪", gz) if gz > 0 else ("右转", "陀螺仪", gz)
        elif abs(yaw_enc) >= 15.0:
            turn, src, rate = ("左转", "编码器差速", yaw_enc) if yaw_enc > 0 else ("右转", "编码器差速", yaw_enc)
        else:
            turn, src, rate = "直行", "—", 0.0
        arrow = {"左转": "◀", "右转": "▶", "直行": "▲"}[turn]
        self.v_turn.set("%s %s" % (arrow, turn))
        self.l_turn.configure(fg="#8ce99a" if turn == "直行" else ("#4dabf7" if turn == "左转" else "#ffa94d"))
        self.s_turn.set("依据：%s" % src)

        # ---------- 角速度 / 累计转角（两条通道互验）----------
        self.v_yaw.set("%+6.1f °/s" % rate)
        self.s_yaw.set("陀螺 %+.0f°  编码 %+.0f°" % (self.gyro_sum, self.yaw_enc_sum))

        # ---------- 灰度 ----------
        self.s_gray.set("偏差 err = %-5s  %s" % (self.err_now, self.gray_now))
        c = self.gray_cv
        c.delete("all")
        for i, ch in enumerate((self.gray_now + "00000000")[:8]):
            x = 3 + i * 12.5
            on = (ch == "1")
            c.create_rectangle(x, 3, x + 10, 16,
                               fill=("#8ce99a" if on else "#242f3a"),
                               outline="#3a4a5a")

        # ---------- 超声波（$U）----------
        if self.us_l is None:
            self.v_us.set("— / —")
            self.l_us.configure(fg="#5a6b7c")
            self.s_us.set("等 $U 帧（开 AUTO 或发 US）")
        else:
            def _us(v):
                return "%d mm" % v if v > 0 else "%d 没测到" % v
            self.v_us.set("%s / %s" % (_us(self.us_l), _us(self.us_r)))
            near = min([v for v in (self.us_l, self.us_r) if v > 0] or [9999])
            # 30cm = A02 的 AV_NEAR_CM；进了就变色，提醒“这就是该避障的距离”
            self.l_us.configure(fg="#ff9f43" if near < 300 else "#d0d8e0")
            age = time.time() - self.us_t
            self.s_us.set("%d 条 · 最近 %.1fs 前%s" %
                          (self.us_n, age, "  ← 进入 30cm!" if near < 300 else ""))

        # ---------- 俯视图 + 方位文字（2026-10-01）----------
        self._update_us_view()

        # ---------- 陀螺仪健康提示（静默失效以前很隐蔽：至少它现在会喊一声）----------
        stale = (time.time() - self.gyro_last_nonzero) > 15.0
        if self.gyro_hist and stale:
            self.s_gyro.set("! gz 一直是 0：查 MPU6050（固件会自己重试唤醒；也可发 MPUINIT）")
            self.l_gyro.configure(fg="#ffb86b")
        else:
            self.s_gyro.set("正=左转(蓝) / 负=右转(橙)")
            self.l_gyro.configure(fg="#7f93a6")

        # ---------- 陀螺仪趋势条 ----------
        c2 = self.canvas
        c2.delete("all")
        w, h = 176, 26
        c2.create_line(0, h // 2, w, h // 2, fill="#33465a")
        vals = list(self.gyro_hist)[-120:]
        if vals:
            step = w / float(max(1, len(vals)))
            for i, v in enumerate(vals):
                vv = max(-60.0, min(60.0, v * 2.0))
                x = i * step + step / 2.0
                y = h / 2.0 - (vv / 60.0) * (h / 2.0 - 1.5)
                c2.create_line(x, h / 2.0, x, y, fill=("#4dabf7" if vv >= 0 else "#ffa94d"))

    def _refresh_gyro(self):
        """（保留旧名字，转发到仪表盘刷新；早期测试脚本用得到）"""
        self._refresh_dash()

    # ==================== 超声波俯视图（2026-10-01）====================
    def _update_us_view(self):
        """文字解释 + 重画俯视图。"""
        if self.us_l is None:
            self.v_us_txt.set("等 $U 帧 …（点「开始接收」）")
        else:
            def one(v, who):
                return "%s：没测到" % who if (v is None or v <= 0) else "%s：%d mm" % (who, v)
            lines = [one(self.us_l, "左"), one(self.us_r, "右")]
            hits = []
            if self.us_l is not None and self.us_l > 0:
                hits.append("左前 %d cm" % (self.us_l // 10))
            if self.us_r is not None and self.us_r > 0:
                hits.append("右前 %d cm" % (self.us_r // 10))
            lines.append("→ " + ("障碍在 " + " / ".join(hits) if hits else
                                 "两侧 1.2m 内无回波（-2=没测到，不是故障 ✓）"))
            if time.time() - self.us_t > 2.0:
                lines.append("⚠ 已 %.0f 秒未更新" % (time.time() - self.us_t))
            self.v_us_txt.set("\n".join(lines))
        self._draw_us()

    def _draw_us(self):
        """画小车俯视图：车头朝上，两颗探头各向外掰 35°（波束半角 15°）。

        几何依据（用户 2026-10-01 提供 + 台面实测）：
          探头间距 70mm、各偏离正前方 35° ⇒ 左看「偏左 20°~50°」、右看「偏右 20°~50°」
          ⇒ **正前方 ±20° 是盲区**（实测：书放车头正前 30cm，两颗都读不到 ✓）

        画法（关键）：测到的距离画成**横贯波束的一段圆弧** ——
          单颗探头只能给“距离”，给不出横向位置 ⇒ 障碍物是“这段弧上某处” ✓
          **故意不画成单个点**（那会假装知道横向位置 ✗）
        刻度：线性（不夸大）；>120cm 不画；<5cm 是实测盲区 ⇒ 标红
        """
        c, W, H = self.us_cv, self.us_W, self.us_H
        c.delete("all")
        cx = W / 2.0
        py = H - 46.0                                          # 探头（车头）线
        span = math.radians(US_HALF_DEG + US_BEAM_DEG)
        R = min(py - 14.0, (W / 2.0 - 10.0) / math.sin(span))   # 最外射线别越出画布

        def px(mm):
            return R * (float(mm) / US_RANGE_MM)              # 线性刻度

        for rr in (300, 600, 900, 1200):                      # 参考弧（只画在波束可能到的角度内）
            r = px(rr)
            c.create_arc(cx - r, py - r, cx + r, py + r, start=22.0, extent=136.0,
                         style="arc", outline="#17242f")
            c.create_text(cx + 3, py - r, text="%dcm" % (rr // 10), anchor="w",
                          fill="#37485a", font=("", 7))

        fresh = (self.us_l is not None) and (time.time() - self.us_t < 2.0)
        for sx, mm in ((-1, self.us_l), (1, self.us_r)):
            ax = cx + sx * px(US_BASE_MM) / 2.0               # 该探头位置
            axis = 90.0 - sx * US_HALF_DEG                    # 屏幕角：左 90+35、右 90−35
            a0, a1 = axis - US_BEAM_DEG, axis + US_BEAM_DEG
            pts = [ax, py]
            for a in (a0, a1):
                pts += [ax + R * math.cos(math.radians(a)), py - R * math.sin(math.radians(a))]
            c.create_polygon(*pts, fill="#101a24", outline="#1f2d3a")
            if mm is not None and mm > 0:
                r = px(min(mm, US_RANGE_MM))
                if not fresh:
                    col = "#4a5a6a"                           # 数据陈旧 ⇒ 灰，别当成实时 ✗
                elif mm < US_MIN_MM:
                    col = "#ff6b6b"                           # <5cm：实测不可靠
                elif mm < US_NEAR_MM:
                    col = "#ff9f43"                           # 进 30cm = A02 的判据
                else:
                    col = "#4dabf7"
                c.create_arc(ax - r, py - r, ax + r, py + r, start=a0,
                             extent=2.0 * US_BEAM_DEG, style="arc", outline=col, width=3)
                ma = math.radians(axis)
                c.create_text(ax + (r + 11) * math.cos(ma), py - (r + 11) * math.sin(ma),
                              text="%d" % mm, fill=col, font=("Consolas", 8, "bold"))

        wf, wr = px(CAR_WF_MM) / 2.0, px(CAR_WR_MM) / 2.0     # 车体（前宽后窄 ⇒ 前驱）
        y1 = min(py + px(CAR_LEN_MM), H - 4.0)
        c.create_polygon(cx - wf, py, cx + wf, py, cx + wr, y1, cx - wr, y1,
                         fill="#16202b", outline="#3d5a75")
        for sx in (-1, 1):                                     # 探头位置（按 7cm 刻度）
            ax = cx + sx * px(US_BASE_MM) / 2.0
            c.create_oval(ax - 2, py - 2, ax + 2, py + 2, fill="#8ce99a", outline="")
        c.create_text(cx, 9, text="↑ 车头方向", fill="#42566a", font=("", 7))
        c.create_text(4, H - 10, text="半径线性刻度 · 虚线之间即 ±15° 波束", anchor="w",
                      fill="#334354", font=("", 7))


def _json_str(s: str) -> str:
    import json
    return json.dumps(s, ensure_ascii=False)


def main() -> int:
    App().mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
