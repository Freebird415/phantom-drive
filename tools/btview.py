# -*- coding: utf-8 -*-
"""BLE 日志实时查看器 —— 自己开、自己看、自动存盘（图形界面）

为什么有这个工具（2026-10-01 用户的意见）：
    `btlog.py` 是命令行版：跑起来只有几行中文提示，**看不见数据长什么样**，
    而且必须由人（或智能体）在终端启动 ⇒ 使用者对链路是否真的在收数据没有直观感受。
    本工具把同一条链路做成**自己双击就能开的窗口**：实时滚动显示每一帧、
    显示帧率/累计帧数、自动写 CSV，随时可以自己取走原始数据。

窗口里能看到什么（每收到一帧显示一行）：
    序号  时刻        灰度(8位)   err    pwm(L/R)  state  dL  dR   说明
    1234  12:03:45.123  00011000  +0.5    8/ 8      1    +3  +2

用法：
    双击仓库根目录的「蓝牙日志.bat」，或在仓库根目录执行  python tools\\btview.py
    ① 备注填一句"这轮在做什么"（会进日志目录名和 note.md）
    ② 点「开始接收」→ 状态栏出现"已连接/帧率"后才算真的在收
    ③ 车随便怎么动都行；要打点就把鼠标点到车旁边的某个时刻按「打点」
    ④ 点「停止」→ 自动写 meta.json / note.md，点「打开日志目录」取数据

数据落盘（与 btlog.py / snapshot.py 完全同一套格式，plot.py 直接能画）：
    logs/<时间>_<备注>/telemetry.csv     ← 所有帧（不因界面过滤而丢）
    logs/<时间>_<备注>/markers.txt       ← 你按「打点」的时刻（如果有）
    logs/<时间>_<备注>/meta.json         ← 传输方式/帧数/设备等

[!] 界面上的"只看动帧"只是**显示过滤**，CSV 里始终是全部帧 ✓
"""
from __future__ import annotations

import asyncio
import collections
import queue
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
        de = DeFramer()

        def on_notify(_ch, data: bytes):
            for line in de.feed(bytes(data)):
                self.n += 1
                self.rec.feed(line)                     # CSV 永远收全部帧
                self.q.put(("frame", line))             # 排版/过滤都在界面线程做

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
        self._log("[i] 已存档：%s" % self.out_dir)
        self._log("[i] 共 %d 帧 → telemetry.csv" % self.n)


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("工创小车 · 蓝牙日志实时接收")
        self.geometry("980x620")
        self.q: "queue.Queue" = queue.Queue()
        self.worker: BleWorker | None = None
        self.markers: list[str] = []
        self.n_disp = 0                     # 屏幕上显示到第几帧（与真实帧号解耦）
        self.gyro_hist = collections.deque(maxlen=400)
        self.gyro_sum = 0.0                 # 累计转角（度，按每帧 30ms 估算）
        self.gyro_last_nonzero = time.time()
        self._gyro_t = datetime.now()
        self._build()
        self.after(120, self._pump)

    # ---------- 界面 ----------
    def _build(self):
        pad = {"padx": 6, "pady": 4}
        top = ttk.Frame(self)
        top.pack(fill="x", **pad)

        ttk.Label(top, text="设备").grid(row=0, column=0, sticky="w")
        self.dev = ttk.Combobox(top, width=22, values=KNOWN)
        self.dev.set(KNOWN[0])
        self.dev.grid(row=0, column=1, sticky="w", padx=4)

        ttk.Label(top, text="备注").grid(row=0, column=2, sticky="w", padx=(12, 0))
        self.note = ttk.Entry(top, width=34)
        self.note.insert(0, "BLE实时")
        self.note.grid(row=0, column=3, sticky="w", padx=4)

        self.btn_start = ttk.Button(top, text="开始接收", command=self.start)
        self.btn_start.grid(row=0, column=4, padx=4)
        self.btn_stop = ttk.Button(top, text="停止", command=self.stop, state="disabled")
        self.btn_stop.grid(row=0, column=5, padx=4)
        ttk.Button(top, text="打开日志目录", command=self.open_dir).grid(row=0, column=6, padx=4)

        opt = ttk.Frame(self)
        opt.pack(fill="x", **pad)
        self.only_move = tk.BooleanVar(value=False)
        ttk.Checkbutton(opt, text="只看动帧（灰度有黑 或 轮子在动 或 电机有命令；只是显示过滤，CSV 仍存全部）",
                        variable=self.only_move).pack(side="left")
        self.autoscroll = tk.BooleanVar(value=True)
        ttk.Checkbutton(opt, text="自动滚动", variable=self.autoscroll).pack(side="left", padx=10)
        ttk.Button(opt, text="打点（记下当前时刻）", command=self.mark).pack(side="left", padx=10)
        ttk.Button(opt, text="清屏", command=lambda: self.txt.delete("1.0", "end")).pack(side="left")

        head = "序号  时刻          灰度        err   pwm L/R    st   dL   dR    gz"

        # ---- 底部：状态栏 + 陀螺仪实时面板（先 pack 底部，再用 Text 占满剩余空间）----
        self.status = tk.StringVar(value="就绪 —— 点「开始接收」；状态栏出现帧率后才算真的在收")
        ttk.Label(self, textvariable=self.status, relief="sunken", anchor="w").pack(
            fill="x", side="bottom")

        gp = tk.Frame(self, bg="#141b22")
        gp.pack(fill="x", side="bottom")
        self.gyro_txt = tk.StringVar(value="陀螺仪 gz = ---   （陀螺仪每 4 拍读一次、±254°/s、2°/s 每格）")
        tk.Label(gp, textvariable=self.gyro_txt, bg="#141b22", fg="#8ce99a",
                 font=("Consolas", 10), anchor="w", padx=8, pady=4).pack(side="left")
        self.canvas = tk.Canvas(gp, width=240, height=32, bg="#0d1216",
                                highlightthickness=0, bd=0)
        self.canvas.pack(side="right", padx=8, pady=4)
        tk.Label(gp, text="正=逆时针/左\n负=顺时针/右", bg="#141b22", fg="#6b7c8c",
                 font=("Consolas", 8), justify="left").pack(side="right")

        # ---- 固定表头（独立控件，不随数据滚动消失）----
        tk.Label(self, text=head, bg="#1b2530", fg="#9fb3c8", font=("Consolas", 10),
                 anchor="w", padx=6, pady=3).pack(fill="x", padx=6, pady=(4, 0))

        box = ttk.Frame(self)
        box.pack(fill="both", expand=True, padx=6, pady=(0, 2))
        self.txt = tk.Text(box, wrap="none", bg="#101418", fg="#d0d8e0",
                           insertbackground="#d0d8e0", font=("Consolas", 10),
                           bd=0, highlightthickness=0, padx=6, pady=2)
        self.txt.pack(side="left", fill="both", expand=True)
        sb = ttk.Scrollbar(box, orient="vertical", command=self.txt.yview)
        sb.pack(side="right", fill="y")
        self.txt.configure(yscrollcommand=sb.set)
        for tag, col in (("ev", "#7fb0ff"), ("warn", "#ffb86b"), ("frame", "#d0d8e0"),
                         ("mark", "#8ce99a")):
            self.txt.tag_configure(tag, foreground=col)

    # ---------- 动作 ----------
    def start(self):
        if self.worker and self.worker.is_alive():
            return
        self.worker = BleWorker(self.q, self.dev.get().strip(),
                                use_addr=":" in self.dev.get(),  # 填 MAC 时按地址连（更稳）
                                note=self.note.get().strip(),
                                min_fps=3.0)
        self.worker.start()
        self.markers = []
        self.btn_start.configure(state="disabled")
        self.btn_stop.configure(state="normal")
        self.status.set("正在连接…（连上后会自动试测 6 秒帧率）")

    def stop(self):
        if self.worker:
            self.worker.stop_flag.set()
            self.status.set("正在停止并写盘…")

    def mark(self):
        t = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        self.markers.append(t)
        self._append("[打点] %s   ← %s" % (t, self.note.get().strip()), "mark")

    def open_dir(self):
        d = self.worker.out_dir if self.worker and self.worker.out_dir else None
        if d and Path(d).exists():
            import subprocess
            subprocess.Popen(["explorer", str(d)])
        else:
            messagebox.showinfo("还没有日志", "先点「开始接收」，日志目录会在那时创建。")

    # ---------- 界面刷新（只在这里碰控件，线程安全）----------
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
                elif kind == "frame":
                    f = frame_fields(payload)
                    if f is None:
                        continue
                    gray, err, pl, pr, st, dl, dr, gz = f
                    if self.only_move.get():                       # 仅显示过滤；CSV 仍是全部帧
                        idle = (gray.strip("0") == ""                    # 灰度全无
                                and lstrip_num(dl) == 0 and lstrip_num(dr) == 0   # 轮子没动
                                and lstrip_num(pl) == 0 and lstrip_num(pr) == 0)  # 也没给电机发命令
                        if idle:
                            continue
                    self.n_disp += 1
                    self._append(fmt_frame(self.n_disp, payload), "frame")
                    # ---- 陀螺仪统计（gz 单位 = 2°/s 每格；每帧名义 30ms）----
                    g = lstrip_num(gz)
                    self.gyro_hist.append(g)
                    self.gyro_sum += g * 2.0 * (1.0 / 33.3)
                    if g:
                        self.gyro_last_nonzero = time.time()
                elif kind == "stat":
                    s = payload
                    self.status.set("状态：%s   累计 %d 帧   当前 %.1f 帧/s   已运行 %.0f s"
                                    % (s.get("state", ""), s.get("n", 0),
                                       s.get("fps", 0.0), s.get("sec", 0.0)))
                elif kind == "done":
                    self.btn_start.configure(state="normal")
                    self.btn_stop.configure(state="disabled")
                    self.status.set("已停止（%s）—— 数据已写入 telemetry.csv" % payload)
                    if self.markers and self.worker and self.worker.out_dir:
                        p = Path(self.worker.out_dir) / "markers.txt"
                        p.write_text("\n".join(self.markers), encoding="utf-8")
                        self._append("[i] 打点已写入 %s" % p.name, "mark")
        except queue.Empty:
            pass
        now = datetime.now()
        if (now - self._gyro_t).total_seconds() >= 0.2:      # 陀螺仪面板每秒刷 5 次就够
            self._gyro_t = now
            self._refresh_gyro()
        self.after(80, self._pump)

    def _refresh_gyro(self):
        if not self.gyro_hist:
            return
        g = self.gyro_hist[-1]
        rate = g * 2.0                                        # gz8 的单位是 2°/s
        if abs(rate) < 3.0:
            how = "静止/直行"
        elif rate > 0:
            how = "左转（逆时针）"
        else:
            how = "右转（顺时针）"
        stale = (time.time() - self.gyro_last_nonzero) > 15.0
        hint = "   [!] 一直是 0：MPU6050 可能没接好/初始化失败" if stale else ""
        self.gyro_txt.set("陀螺仪 gz = %+6.1f °/s   %-10s  累计转角 %+7.1f°   (每帧 30ms 估算)%s"
                          % (rate, how, self.gyro_sum, hint))
        # 趋势条：最近 ~120 帧（约 3.6 秒），±60°/s 满量程
        c = self.canvas
        c.delete("all")
        w, h = 240, 32
        c.create_line(0, h // 2, w, h // 2, fill="#33465a")
        vals = list(self.gyro_hist)[-120:]
        if not vals:
            return
        step = w / float(max(1, len(vals)))
        for i, v in enumerate(vals):
            vv = max(-60.0, min(60.0, v * 2.0))
            x = i * step + step / 2.0
            y = h / 2.0 - (vv / 60.0) * (h / 2.0 - 2.0)
            c.create_line(x, h / 2.0, x, y, fill=("#4dabf7" if vv >= 0 else "#ffa94d"))


def main() -> int:
    App().mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
