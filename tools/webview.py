#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""webview.py —— 蓝牙实时日志（**网页界面版**）

为什么有它（2026-10-01）：
    tkinter 版（btview.py）在 Windows 上用 ttk 时，控件与标题栏/窗口边框的
    深色适配很别扭（原生主题忽略颜色、DWM 深色模式又要重绘才生效）✗ ——
    换成浏览器界面后：外观完全可控、布局更舒展，而且**能被自动截图验证** ✓

怎么用：
    python tools/webview.py                 # 起服务并自动打开浏览器
    python tools/webview.py --no-browser    # 只起服务
    浏览器地址：http://127.0.0.1:8777

分工（刻意做成“界面只负责显示”）：
    采集/解帧   → btlog.DeFramer（与 btlog/btview 完全同一套，格式不会漂 ✓）
    落盘/摘要   → telemetry.Recorder + shared_log（复用既有机制 ✓）
    片段导出    → btview 里的 slice_session_csv / write_derived（同一套实现 ✓）
    本文件      → 本地 HTTP 服务 + SSE 推送 + 上面这些的编排
"""
from __future__ import annotations

import argparse
import asyncio
import collections
import json
import queue
import sys
import threading
import time
import urllib.parse
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import btlog                    # noqa: E402  DeFramer / NOTIFY / KNOWN
import telemetry                # noqa: E402  parse_line / Recorder / new_experiment_dir
import shared_log               # noqa: E402  摘要（共享日志）
import btview                   # noqa: E402  复用片段导出/只读保护等纯函数（导入不会开窗 ✓）

HTML_FILE = HERE / "webui.html"
DEFAULT_PORT = 8777


class Server(ThreadingHTTPServer):
    """⚠️ 必须关掉 allow_reuse_address（2026-10-01 实踩）：

    Python 的 HTTPServer 默认 allow_reuse_address = 1，而在 **Windows 上**这意味着
    **两个进程可以同时绑同一端口**（Linux 语义不同 ✗✗）⇒ 双击两次 bat 会起出两个服务，
    两个都活着、连接被随机分给其中一个（实测：确实出现了 2 个 pythonw ✓）
    关掉后，第二次 bind 会真的报 WSAEADDRINUSE ⇒ 走到“已有一个在跑”分支，只开浏览器 ✓
    （监听端口的 TIME_WAIT 不会挡住重新绑定 —— 只有已建立的连接才会 TIME_WAIT ✓）"""

    allow_reuse_address = False
    daemon_threads = True
    _exit_timer = None
    hub = None

    def arm_exit(self, delay: float = 5.0):
        """网页关掉 ⇒ 退出服务（2026-10-01 用户要求）。

        ⚠️ 加固（实测踩过）：**刷新页面也会触发 beforeunload** ✗，光靠“新页面连上来就取消”
           不够稳（时序不对就把服务弄死了 ✗）⇒ 定时器到点时**再查一次有没有页面连着**：
           还有 SSE 订阅者 ⇒ 说明只是刷新/跳转，不退 ✓；真没人了 ⇒ 退 ✓
        """
        self.cancel_exit()

        def _fire():
            try:
                with self.hub.lock:
                    n = len(self.hub.subs)
            except Exception:
                n = 1                       # 查不到就保守不退 ✓
            if n == 0:
                self.shutdown()

        t = threading.Timer(delay, _fire)
        t.daemon = True
        self._exit_timer = t
        t.start()

    def cancel_exit(self):
        if self._exit_timer is not None:
            self._exit_timer.cancel()
            self._exit_timer = None


# ============================== 数据中枢 ==============================
class Hub:
    """所有订阅者共用的状态：最新帧、状态、日志行。

    设计：每个浏览器连接一个 queue（SSE 推送）；Hub 只做转发 + 保存“最近的状态快照”，
    让刚打开的页面立刻能看到当前值（不用等下一帧 ✓）。
    """

    def __init__(self):
        self.lock = threading.Lock()
        self.subs: list[queue.Queue] = []
        self.frames: list[dict] = []          # 待推送批次（每 ~100ms 冲一次）
        self.last_flush = time.time()
        self.n_recv = 0                       # 收到多少帧（含未记录时）
        self.n_rec = 0                        # ⭐ 本会话【记录】了多少帧（每次开始接收归 1 ✓）
        self.n_us = 0
        self.t0 = time.time()
        self.state = "idle"                   # idle / connecting / live / error
        self.msg = "就绪"
        self.session_dir: Path | None = None
        self.rec = None                       # telemetry.Recorder
        self.note = ""
        self.markers: list[dict] = []
        self.last_us = [None, None]           # 左右超声波最近值
        self.t_last_live = 0.0                # 最近一次“状态转 live”的时刻（页面重连时用 ✓）
        self.last_us_t = 0.0
        self.last_params = {}
        self.events: list[dict] = []
        self.frame_ms = 20.0
        self.viewing = ""                       # 正在浏览的历史日志名（空=实时）✓
        self.monitoring = False                # ⭐ 监测模式（2026-10-01 用户要求）：只显示、不写文件 ✓
        # ⭐ 黑匣子：监测期间把原始行缓在内存里（≈65 s @50 行/s）——
        #    现象出现时按空格，能把【之前没来得及点】的那几十秒一起存进 CSV ✓
        self.recent = collections.deque(maxlen=3400)
        self.drop_us = 0                      # $U 的 -6/-3 次数（驱动拦下，不是故障）
        self.n_bad_us = 0                     # $U 的 -1/-2 次数

    # ---- 读一份历史 CSV 并推给前端（只推最后 N 帧，够看趋势 ✓）----
    def load_csv_frames(self, path, limit: int = 4000) -> int:
        import csv as _csv
        rows = []
        total = 0
        ul = ur = None                                   # 该帧之前最近一次的 $U 读数 ✓
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                for r in _csv.DictReader(f):
                    if r.get("kind") == "U":            # ⭐ 超声波行：记下最近值（不单独成行）✓
                        try:
                            l0 = int(float(r.get("l_mm") or -2))
                            r0 = int(float(r.get("r_mm") or -2))
                            if l0 not in (-6, -3):
                                ul = l0
                            if r0 not in (-6, -3):
                                ur = r0
                        except (TypeError, ValueError):
                            pass
                        continue
                    if r.get("kind") != "T":
                        continue
                    total += 1
                    rows.append({
                        "i": 0,                     # 下面统一编号 ✓
                        "ms": int(float(r.get("ms") or 0)),
                        "gray": "".join(str(int(float(r.get("s%d" % k) or 0))) for k in range(8)),
                        "err": float(r.get("err") or 0),
                        "pl": int(float(r.get("pwm_l") or 0)),
                        "pr": int(float(r.get("pwm_r") or 0)),
                        "st": int(float(r.get("state") or 0)),
                        "dl": int(float(r.get("dl") or 0)),
                        "dr": int(float(r.get("dr") or 0)),
                        "gz": int(float(r.get("gz") or 0)),
                        "ul": ul, "ur": ur,
                    })
        except OSError:
            return 0

        # ⭐ 帧间隔要按【这份文件自己】的 ms 差分算（2026-10-01 实踩）：
        #    以前用 self.frame_ms（还留着实时流的值，实测=24ms ✗）去除 ms
        #    ⇒ 序号出现重复（335/335、340/340…）且与导出片段的“第几帧”口径对不上 ✗
        deltas = [rows[i]["ms"] - rows[i - 1]["ms"] for i in range(1, len(rows))]
        deltas = sorted(x for x in deltas if 0 < x <= 200)
        if deltas:
            self.frame_ms = float(deltas[len(deltas) // 2])    # 中位数 = 抗丢帧 ✓

        rows = rows[-limit:]
        # ⭐ 序号 = “第几帧”（1 起，**绝对口径**：与 export_clip 数 T 行的口径完全一致 ✓）
        #    超出 limit 被截掉的前段也要计数，否则大日志（>4000 帧）导出会偏 ✗
        base = total - len(rows)
        for k, r in enumerate(rows, 1):
            r["i"] = base + k
        for i in range(0, len(rows), 200):
            self.push("frames", rows[i:i + 200])
        return len(rows)

    # ---- 订阅（SSE）----
    def sub(self) -> queue.Queue:
        q: queue.Queue = queue.Queue(maxsize=400)
        with self.lock:
            self.subs.append(q)
        return q

    def unsub(self, q: queue.Queue):
        with self.lock:
            if q in self.subs:
                self.subs.remove(q)

    def push(self, kind: str, payload):
        with self.lock:
            subs = list(self.subs)
        for q in subs:
            try:
                q.put_nowait((kind, payload))
            except queue.Full:
                pass

    def snapshot(self) -> dict:
        return {
            "state": self.state, "msg": self.msg, "n_recv": self.n_recv, "n_us": self.n_us,
            "secs": time.time() - self.t0, "session": str(self.session_dir or ""),
            "recording": self.rec is not None,      # 按钮启用状态按它来（别让“停止接收”白点 ✓）
            "viewing": self.viewing,                # 打开的是哪份历史日志（空=实时）✓
            "monitoring": self.monitoring,          # 监测中（只显示不写文件）✓
            "note": self.note, "markers": self.markers[-80:],
            # ⭐ 导入进来的那份（目录名带 _import_）⇒ 允许直接删 ✗
            #    （2026-10-01 实踩：刚导入完想删掉，却发现按钮是灰的 ✗
            #      —— “浏览历史时禁止删”那条规则本意是保护从磁盘打开的真日志 ✓）
            "imported": bool(self.session_dir and "_import_" in self.session_dir.name),
            "us": self.last_us, "us_age": (time.time() - self.last_us_t) if self.last_us_t else None,
            "params": self.last_params, "events": self.events[-40:],
            "frame_ms": self.frame_ms, "us_dropped": self.drop_us, "us_bad": self.n_bad_us,
        }

    # ---- 一帧 $T ⇒ 前端要的 dict（on_line 与“监测回溯”共用，避免两处写重复 ✓）----
    #    ⭐ ul/ur = 【该帧时刻最近一次的 $U 读数】（$U 约 4Hz，比 $T 慢很多 ⇒ “保留上一个值”
    #       才是正确语义 ✓）；表里就显示这两个 ✓（2026-10-02 用户要求：日志里也要有超声波 ✗）
    def _frame_dict(self, data, i, ul=None, ur=None):
        if ul is None:
            ul = self.last_us[0]
        if ur is None:
            ur = self.last_us[1]
        return {
            "i": i,
            "ms": data["ms"], "gray": "%d%d%d%d%d%d%d%d" % tuple(data["s%d" % k] for k in range(8)),
            "err": data["err"], "pl": data["pwm_l"], "pr": data["pwm_r"], "st": data["state"],
            "dl": data.get("dl", 0), "dr": data.get("dr", 0), "gz": data.get("gz", 0),
            "ul": ul, "ur": ur,
        }

    # ---- 收到一行遥测 ----
    def on_line(self, line: str):
        parsed = telemetry.parse_line(line)
        if not parsed:
            return
        kind, data = parsed

        # ⭐ 监测中：先把原始行存进黑匣子（按空格开始记录时会把它们一起落到 CSV ✓）
        if self.monitoring:
            self.recent.append(line)

        if self.rec is not None:
            try:
                self.rec.feed(line)
            except Exception:
                pass

        if kind == "T":
            self.n_recv += 1
            # ⭐ 序号列=本会话第几帧（每次开始接收从 1 开始 ✓）；它也正好是导出片段用的口径
            #    （slice_session_csv 数的是 T 行 ✓）
            #    监测时也照样计数（否则监测期间序号列全是 0 ✗）
            if self.rec is not None or self.monitoring:
                self.n_rec += 1
            n_this = self.n_rec
            # ⚠️ 浏览历史日志时【不推实时帧】
            if self.viewing:
                return
            # ⭐ 没在记录/监测/浏览时，根本不用往浏览器推帧（省带宽 ✓）
            if self.rec is None and not self.monitoring:
                return
            self.frames.append(self._frame_dict(data, n_this))
            if time.time() - self.last_flush >= 0.1 or len(self.frames) >= 24:
                self.push("frames", self.frames)
                self.frames = []
                self.last_flush = time.time()
        elif kind == "U":
            l, r = data["l_mm"], data["r_mm"]
            if l not in (-6, -3):
                self.last_us[0] = l
            else:
                self.drop_us += 1
            if r not in (-6, -3):
                self.last_us[1] = r
            else:
                self.drop_us += 1
            if l < 0 and l not in (-6, -3):
                self.n_bad_us += 1
            if r < 0 and r not in (-6, -3):
                self.n_bad_us += 1
            self.n_us += 1
            self.last_us_t = time.time()
            self.push("us", {"l": l, "r": r, "t": data["ms"]})
        elif kind == "E":
            ev = {"ms": data["ms"], "code": data["code"], "note": data.get("note", "")}
            self.events.append(ev)
            self.push("event", ev)
        elif kind == "P":
            self.last_params = data
            self.push("params", data)


# ============================== BLE 采集线程 ==============================
class BleWorker(threading.Thread):
    """后台线程：跑 asyncio + bleak，**失败就每 5 秒重试**。

    ⚠️ 2026-10-01 用户要求：不要“连接蓝牙”按钮，开 bat 就自动连；
       状态只在【已连接 / 未连接 / 连接中】三态间切；
       连接中每次尝试 5 秒，连不上就转“未连接”，再每 5 秒重试 ⇒ 两态循环显示 ✓
       一旦连上就保持（除非断开/用户退出）✓
    """

    RECONNECT_S = 5.0        # 未连接/连接失败后，隔多久再试（用户指定 5 秒 ✓）
    ATTEMPT_S   = 5.0        # 单次扫描/连接的超时（用户指定 5 秒 ✓）

    def __init__(self, hub: Hub, device: str | None = None, addr: str | None = None):
        super().__init__(daemon=True)
        self.hub, self.addr = hub, addr
        self.device = device or btlog.KNOWN[0]
        self.stopped = threading.Event()

    def stop(self):
        self.stopped.set()

    def run(self):
        try:
            asyncio.run(self._main())
        except Exception as e:                                  # noqa: BLE001
            self.hub.state = "idle"
            self.hub.msg = "蓝牙线程异常：%s" % e
            self.hub.push("status", self.hub.snapshot())

    async def _main(self):
        from bleak import BleakClient, BleakScanner
        hub = self.hub

        while not self.stopped.is_set():
            # ---------- ① 扫描（≤5 秒）----------
            hub.state = "connecting"
            hub.msg = "连接中…（扫描 %s）" % self.device
            hub.push("status", hub.snapshot())
            dev = None
            try:
                if self.addr:
                    dev = self.addr
                else:
                    dev = await asyncio.wait_for(
                        BleakScanner.find_device_by_name(self.device, timeout=self.ATTEMPT_S),
                        timeout=self.ATTEMPT_S + 1.0)
            except Exception:
                dev = None
            if self.stopped.is_set():
                break
            if dev is None:
                hub.state = "idle"
                hub.msg = "未连接（%d 秒后重试）" % int(self.RECONNECT_S)
                hub.push("status", hub.snapshot())
                await asyncio.sleep(self.RECONNECT_S)
                continue

            # ---------- ② 连接并收数据 ----------
            hub.msg = "连接中…（正在建链）"
            hub.push("status", hub.snapshot())
            try:
                async with BleakClient(dev, timeout=self.ATTEMPT_S) as cl:
                    de = btlog.DeFramer()

                    def on_notify(_ch, data: bytes, de=de):
                        for line in de.feed(bytes(data)):
                            hub.on_line(line)
                        # ⚠️ 正在【浏览历史日志/导入的文件】时不要让实时流改写帧间隔 ✗
                        #    （2026-10-01 实踩：导入一份 40ms/帧 的旧日志，实时流的 23.4ms
                        #      把它盖掉 ⇒ 界面按 20ms 算速度 ⇒ 历史日志的速度/转角全错 ✗）
                        if not hub.viewing:
                            hub.frame_ms = de.frame_ms

                    await cl.start_notify(btlog.NOTIFY, on_notify)
                    hub.state = "live"
                    hub.msg = "已连接，接收中…"
                    hub.t0 = time.time()
                    hub.t_last_live = time.time()
                    hub.push("status", hub.snapshot())
                    while not self.stopped.is_set():
                        await asyncio.sleep(0.2)
                    try:
                        await cl.stop_notify(btlog.NOTIFY)
                    except Exception:
                        pass
            except Exception as e:                              # noqa: BLE001
                hub.state = "idle"
                hub.msg = "连接失败：%s（%d 秒后重试）" % (e, int(self.RECONNECT_S))
                hub.push("status", hub.snapshot())
                await asyncio.sleep(self.RECONNECT_S)
                continue

            if self.stopped.is_set():
                break
            # 掉线（模块断电/走远）⇒ 也继续重试 ✓
            hub.state = "idle"
            hub.msg = "未连接（%d 秒后重试）" % int(self.RECONNECT_S)
            hub.push("status", hub.snapshot())
            await asyncio.sleep(self.RECONNECT_S)

        hub.state = "idle"
        hub.msg = "已停止"
        hub.push("status", hub.snapshot())


# ============================== 录制 / 导出 编排 ==============================
def start_record(hub: Hub, note: str = "", prefill: bool = False):
    """开始记录。

    ⚠️ 备注已取消（2026-10-01 用户要求“不需要备注”）⇒ 目录名只用时间戳 ✓
    ⭐ prefill=True（监测模式按空格）：把黑匣子里最近几十秒的原始行先落进 CSV ✓
       —— 用户要求：“监测时出现现象但没来得及开始记录” ⇒ 按一下空格就能连前面一起存 ✓
    """
    if hub.rec is not None:
        return
    hub.viewing = ""                       # 开始新记录 ⇒ 退出“浏览历史”模式 ✓
    hub.n_rec = 0                          # ⭐ 序号从 1 重新开始（用户 2026-10-01 要求 ✓）
    note = (note or "").strip()
    out = telemetry.new_experiment_dir(note)
    hub.session_dir = out
    hub.note = note
    hub.rec = telemetry.Recorder(out, note, port="BLE", baud=0, seconds=0)
    hub.markers = []
    hub.n_recv = 0
    hub.n_us = 0
    hub.t0 = time.time()
    hub.events = []
    hub.frames = []

    # ⭐ 黑匣子回溯：先写进 CSV，再把对应帧按序号 1..K 推给前端
    #    （前端会先清表 ⇒ 屏幕上看到的 = CSV 里存的那一份，序号也完全对得上 ✓）
    if prefill and hub.recent:
        lines = list(hub.recent)
        k, batch = 0, []
        ul = ur = None                                  # 回溯段也要按“当时”的超声值走 ✓
        for ln in lines:
            try:
                hub.rec.feed(ln)
            except Exception:
                continue
            pr = telemetry.parse_line(ln)
            if not pr:
                continue
            if pr[0] == "U":
                l0, r0 = pr[1]["l_mm"], pr[1]["r_mm"]
                if l0 not in (-6, -3):
                    ul = l0
                if r0 not in (-6, -3):
                    ur = r0
            elif pr[0] == "T":
                k += 1
                batch.append(hub._frame_dict(pr[1], k, ul, ur))
        hub.n_rec = k
        hub.n_recv = k
        for i in range(0, len(batch), 200):
            hub.push("frames", batch[i:i + 200])
    hub.push("status", hub.snapshot())


def stop_record(hub: Hub, make_share: bool = True):
    """停止记录：关 CSV → 自动生成共享摘要（与 btview 默认行为一致）。"""
    if hub.rec is None:
        return None
    try:
        hub.rec.close()
    except Exception:
        pass
    hub.rec = None
    mk = hub.session_dir / "markers.txt"
    if hub.markers and hub.session_dir:
        try:
            mk.write_text("\n".join("%d\t%s" % (m["n"], m["name"]) for m in hub.markers), encoding="utf-8")
        except Exception:
            pass
    out = None
    if make_share and hub.session_dir:
        # ⭐ 摘要名用【目录名】（带时间戳）—— 不要用 note：
        #    监测模式记录时 note="monitor" ✗ ⇒ 摘要会变成一堆 共享_monitor-2/-3.md ✗
        #    （普通记录 note="" 时行为不变 ✓）
        out = auto_share(hub.session_dir, hub.session_dir.name)
    hub.push("status", hub.snapshot())
    return out


def auto_share(log_dir: Path, name: str, who: str = "", note_extra: str = "自动生成（录制结束时）"):
    """生成可入库的小摘要（与 btview.auto_share 同一套逻辑；同名自动加 -2/-3）。"""
    try:
        rows = shared_log.load(log_dir)
        if not rows:
            return None
        md = shared_log.build_md(log_dir, name, who, "", note_extra, "", rows)
        out = shared_log.SHARED / ("%s%s.md" % (shared_log.PREFIX, name))
        if out.exists():
            for i in range(2, 100):
                cand = shared_log.SHARED / ("%s%s-%d.md" % (shared_log.PREFIX, name, i))
                if not cand.exists():
                    out = cand
                    break
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(md, encoding="utf-8")
        return out
    except Exception as e:                                       # noqa: BLE001
        print("[!] 自动生成共享版失败（不影响记录）：%s" % e)
        return None


def export_clip(hub: Hub, a: int, b: int, name: str, who: str = ""):
    """把母日志的第 a~b 帧导出成片段目录（telemetry/derived/meta/note + 只读）。"""
    src_dir = hub.session_dir
    if not src_dir or not (src_dir / "telemetry.csv").exists():
        return None, "还没有日志"
    a, b = max(1, int(a)), int(b)
    if b < a:
        a, b = b, a

    base = btview.safe_dir_name(name) or "片段"
    out = btview.unique_dir(src_dir / ("片段_%s" % base))
    try:
        out.mkdir(parents=True, exist_ok=True)
        n = btview.slice_session_csv(src_dir / "telemetry.csv", a, b, out / "telemetry.csv")
        st = btview.write_derived(src_dir / "telemetry.csv", out / "derived.csv", a, b)
        (out / "meta.json").write_text(json.dumps({
            "note": hub.note, "kind": "clip", "name": base, "transport": "BLE",
            "clip": {"from": a, "to": b, "frames": n, "src": str(src_dir)},
            "saved": time.strftime("%Y-%m-%dT%H:%M:%S"),
        }, ensure_ascii=False, indent=2), encoding="utf-8")
        # note.md：把摘要里最常用的几项落到片段目录（字段定义可随文件走 ✓）
        rows = shared_log.load(out)
        s = shared_log.summarize(rows) if rows else {}
        (out / "note.md").write_text(
            "# 片段 %s\n\n- 来源：`%s` 第 %d~%d 帧（共 %d 帧）\n- 备注：%s\n\n"
            "## 汇总\n\n| 指标 | 值 |\n|---|---|\n"
            "| 时长 | %.1f s |\n| 均速 | %.1f mm/s |\n| 峰值速度 | %.1f mm/s |\n"
            "| 里程 | %.0f mm |\n| \\|err\\| 均值 | %.2f |\n| \\|err\\|≤1 占比 | %.1f%% |\n"
            "| 陀螺累计 | %.0f° |\n| 编码累计 | %.0f° |\n"
            % (base, src_dir.name, a, b, n, hub.note,
               s.get("dur", 0), s.get("v_mean", 0), s.get("v_max", 0), s.get("dist_mm", 0),
               s.get("err_mean", 0), s.get("err_le1", 0), s.get("yaw_gyro", 0), s.get("yaw_enc", 0)),
            encoding="utf-8")
    except Exception as e:                                       # noqa: BLE001
        return None, "导出失败：%s" % e

    for f in out.rglob("*"):                                     # 片段也设只读 ✓
        if f.is_file():
            btview._set_readonly(f, True)
    auto_share(out, base, who, "由导出片段自动生成")
    return out, "已导出第 %d~%d 帧（共 %d 帧）" % (a, b, n)


# ============================== HTTP 服务 ==============================
def start_worker(server, hub: Hub, device=None, addr=None):
    """启一个 BleWorker（会自动替换旧的）—— main() 和 HTTP 接口共用 ✓

    ⚠️ 2026-10-01 实踩：原来是 Handler 的实例方法（用 self.server ✗），
       而 main() 里用 `Handler._start_worker(Handler, …)` 直接调用 ⇒ self 是【类】⇒ 没有 .server ✗
       ⇒ 抽成普通函数，两条路径都走它 ✓"""
    old = getattr(server, "worker", None)
    if old is not None:
        old.stop()
    w = BleWorker(hub, device, addr)
    server.worker = w
    w.start()
    return w


class Handler(BaseHTTPRequestHandler):
    hub: Hub = None                                          # 由 main 注入
    server_version = "phantom-webview"

    def log_message(self, fmt, *args):                       # 静音（控制台只留关键信息）
        pass

    # ---- 工具 ----
    def _send(self, code: int, body: bytes, ctype: str):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, obj, code=200):
        self._send(code, json.dumps(obj, ensure_ascii=False).encode("utf-8"),
                   "application/json; charset=utf-8")

    def _read_body(self) -> bytes:
        """只读一次请求体（⚠️ rfile 是流，读过就没了 ✗）"""
        try:
            n = int(self.headers.get("Content-Length") or 0)
            return self.rfile.read(n) if n else b""
        except Exception:
            return b""

    def _body_json(self, raw: bytes | None = None) -> dict:
        try:
            raw = self._read_body() if raw is None else raw
            return json.loads(raw.decode("utf-8")) if raw else {}
        except Exception:
            return {}

    # ---- 路由 ----
    def do_GET(self):
        p = self.path.split("?")[0]
        # 主页同时挂在几个路径下（/ 与 /lpynb —— 后者是给自己用的短地址，好记 ✓）
        if p in ("/", "/index.html", "/lpynb", "/LPYNB"):
            try:
                self._send(200, HTML_FILE.read_bytes(), "text/html; charset=utf-8")
            except FileNotFoundError:
                self._send(500, b"webui.html missing", "text/plain")
        elif p == "/api/state":
            self._json(self.hub.snapshot())
        elif p == "/api/logs":
            self._send_logs()          # 列表用 GET 更合理 ✓（前端就是这么调的）
        elif p == "/api/stream":
            self._sse()
        else:
            self._send(404, b"not found", "text/plain")

    def _send_logs(self):
        """「打开日志」列表：**母日志 + 所有片段（含嵌套）都列出来** ✓

        （2026-10-01 用户要求：“让日志能全部都能选到，并且导入后可二次截取”✓
          之前只 glob 了一层 `2026-*` ⇒ 片段/子片段完全看不到 ✗）"""
        root = telemetry.repo_root() / "logs"
        cands = [x for x in root.glob("2026-*") if x.is_dir()]
        cands += [x for x in root.glob("**/片段_*") if x.is_dir()]      # 片段（任意深度）✓
        out = []
        seen = set()
        for d in sorted(cands, reverse=True)[:120]:
            rel = str(d.relative_to(root)).replace("\\", "/")
            if rel in seen or not (d / "telemetry.csv").exists():
                continue
            seen.add(rel)
            n = 0
            try:
                with open(d / "telemetry.csv", encoding="utf-8", errors="replace") as f:
                    n = sum(1 for l in f if l.startswith("T,"))
            except OSError:
                n = 0
            out.append({"name": rel, "frames": n, "clip": "片段_" in d.name})
        self._json({"ok": True, "list": out}, )

    def do_POST(self):
        p = self.path.split("?")[0]
        # ⚠️ 2026-10-01 实踩（用户报“导入一直卡在正在导入”✗）：
        #    这里原先是 d = self._body_json()，它【把请求体读走了】
        #    ⇒ 后面 /api/import 再 self.rfile.read() 只能读到空 ✗
        #    ⇒ 修：请求体只读一次，既给 JSON 解析、也给上传分支用 ✓
        raw = self._read_body()
        d = self._body_json(raw)
        hub = self.hub
        if p == "/api/start":
            hub.note = (d.get("note") or "").strip()      # 备注已取消 ✓（目录名只用时间戳）
            # ⭐ 用户要求：**没连蓝牙就不允许开始接收** ✗（按钮会变灰，这里再拦一道 ✓）
            #    ⚠️ 2026-10-01 实踩：以前写成允许 "connecting" ✗ ⇒ 记录开始了但没有帧 ✗
            #       ⇒ 界面看着“日志栏没东西”（就是用户报的那个 ✗）
            if hub.state != "live":
                self._json({"ok": False, "msg": "蓝牙还没连上（会自动重连，约 5 秒一次）—— 等显示「蓝牙已连接」再点"})
                return
            start_record(hub, "monitor" if hub.monitoring else hub.note, prefill=hub.monitoring)
            self._json({"ok": True, "session": str(hub.session_dir or "")})
        elif p == "/api/monitor":
            # ⭐ 监测模式（2026-10-01 用户要求）：
            #    开：一切正常显示，但**不写任何文件**（不占空间 ✓ 方便调试 ✓）
            #    关：全部清零、回初始态 ✓（若监测途中在记录 ⇒ 先把记录存下来，别丢数据 ✓）
            if d.get("on"):
                if hub.state != "live":
                    self._json({"ok": False, "msg": "蓝牙还没连上 —— 等显示「蓝牙已连接」再点「监测」"})
                    return
                if hub.rec is not None:
                    self._json({"ok": False, "msg": "正在记录 —— 先「停止接收」再开监测"})
                    return
                hub.monitoring = True
                hub.viewing = ""
                hub.session_dir = None
                hub.note = ""
                hub.n_rec = hub.n_recv = hub.n_us = 0
                hub.t0 = time.time()
                hub.markers, hub.events, hub.frames = [], [], []
                hub.recent.clear()
                hub.last_us = [None, None]
                hub.msg = "已连接，接收中…"        # 监测的提示交给前端前缀显示（避免“监测中·监测中”重复 ✗）
                hub.push("status", hub.snapshot())
                self._json({"ok": True, "msg": "已开始监测：数据只显示不落盘 ✓ 按空格可随时开始/停止记录"})
            else:
                out = stop_record(hub) if hub.rec is not None else None
                hub.monitoring = False
                hub.recent.clear()
                hub.frames, hub.markers, hub.events = [], [], []
                hub.n_rec = hub.n_recv = hub.n_us = 0
                hub.session_dir, hub.note, hub.viewing = None, "", ""
                hub.last_us = [None, None]
                hub.msg = "就绪"
                hub.push("status", hub.snapshot())
                self._json({"ok": True, "msg": "已停止监测，数据已清零" +
                            ("；途中那份记录已保存：" + str(out) if out else "")})
        elif p == "/api/connect":
            # ⭐ 只连蓝牙、不开始记录（2026-10-01 用户要求把“连接”和“接收”拆开 ✓）
            if hub.state not in ("live", "connecting"):
                self._start_worker((d.get("device") or "").strip() or None, d.get("addr") or None)
            self._json({"ok": True})
        elif p == "/api/disconnect":
            # 断开蓝牙（正在记录就先停掉，避免丢帧 ✗）
            if hub.rec is not None:
                stop_record(hub)
            w = getattr(self.server, "worker", None)
            if w is not None:
                w.stop()
                self.server.worker = None
            # 会话目录保留（已录的数据不动 ✓）—— 只清打点和连接状态
            hub.markers = []                       # 断开了 ⇒ 打点不带到下一个会话 ✓
            hub.push("status", hub.snapshot())
            self._json({"ok": True})
        elif p == "/api/stop":
            out = stop_record(hub)
            self._json({"ok": True, "share": str(out or "")})
        elif p == "/api/mark":
            n = int(d.get("n") or hub.n_recv)
            hub.markers.append({"n": n, "name": (d.get("name") or "").strip()})
            self._json({"ok": True, "markers": hub.markers[-80:]})
        elif p == "/api/unmark":
            n = int(d.get("n") or 0)
            hub.markers = [m for m in hub.markers if m["n"] != n]
            self._json({"ok": True, "markers": hub.markers[-80:]})
        elif p == "/api/export":
            out, msg = export_clip(hub, d.get("a") or 1, d.get("b") or 0,
                                   d.get("name") or "片段", d.get("who") or "")
            self._json({"ok": out is not None, "msg": msg, "dir": str(out or "")})
        elif p == "/api/clear":
            # ⭐ 「清空日志」= 清界面 + 退出浏览态（2026-10-01 用户要求：
            #    清空/删除后事件不再记录、超声图不显示、探头数字不显示，全部回初始态 ✓）
            #    只清 hub.viewing / 计数/读点，**磁盘上的 CSV 不动** ✓
            if hub.rec is not None or hub.monitoring:
                self._json({"ok": False, "msg": "记录/监测中不能清空"})
                return
            hub.viewing = ""                 # 退出“浏览历史/导入” ⇒ 实时帧不再推给界面 ✓
            hub.frames = []
            hub.events = []
            hub.markers = []
            hub.last_us = [None, None]
            hub.n_recv = hub.n_us = hub.n_rec = 0
            hub.push("status", hub.snapshot())
            self._json({"ok": True, "msg": "已清空日志显示（磁盘上的 CSV 不受影响 ✓）"})
        elif p == "/api/discard":
            if hub.rec is not None:
                stop_record(hub, make_share=False)
            d0 = hub.session_dir
            ok = False
            if d0 and d0.exists():
                try:
                    # ⭐ 先把它下面的子片段【抬到上一层】再删（btview 同款护拦 ✓）
                    #    否则母日志一删，里面导出的片段全跟着没 ✗（《说明书》§4.4 写的就是这个行为 ✓）
                    btview.promote_children(d0)
                    btview._rmtree_force(d0)
                    ok = True
                except Exception as e:                            # noqa: BLE001
                    self._json({"ok": False, "msg": str(e)})
                    return
            hub.session_dir = None
            hub.markers = []                    # ⭐ 日志删了 ⇒ 打点也一并清掉 ✓
            hub.n_recv = 0
            hub.events = []                     # ⭐ 事件/读数也归零（用户 2026-10-01 要求 ✓）
            hub.last_us = [None, None]
            hub.viewing = ""
            hub.push("status", hub.snapshot())
            self._json({"ok": ok, "msg": "已删除本次日志" if ok else "没有可删的日志"})
        elif p == "/api/quit":
            # 网页上的「退出服务」：告诉浏览器一声，然后关掉本地服务
            # （因为 bat 用 pythonw 后台启动，没有终端窗口可关 ⇒ 必须留一个退出入口 ✓）
            self._json({"ok": True})
            threading.Timer(0.4, self.server.shutdown).start()
        elif p == "/api/bye":
            # 网页关掉 ⇒ ① 若在记录：**自动停止并保存全部**（用户 2026-10-01 要求 ✓）
            #           ② 预约退出服务（5 秒宽限，刷新页面会取消 ✓）
            if hub.rec is not None:
                out = stop_record(hub)          # 含自动生成共享摘要 ✓
                hub.msg = "页面已关闭 ⇒ 已自动停止并保存" + ("：" + str(out) if out else "")
            self.server.arm_exit(5.0)
            self._json({"ok": True})
        elif p == "/api/logs":
            self._send_logs()
        elif p == "/api/import":
            # ⭐ 拖入框上传的日志（原始 CSV 字节 + ?name=文件名）—— 存成新会话后可再截取 ✓
            if hub.rec is not None:
                self._json({"ok": False, "msg": "正在记录 —— 先「停止接收」再导入"})
                return
            if hub.monitoring:
                self._json({"ok": False, "msg": "监测中 —— 先「停止监测」再导入"})
                return
            fn = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query).get("name", ["log.csv"])[0]
            if not raw:
                self._json({"ok": False, "msg": "上传内容为空（没接到文件数据）"})
                return
            safe = "".join(c for c in fn if c not in '\\/:*?"<>|')[:24] or "log.csv"
            d0 = telemetry.new_experiment_dir("import_" + safe.replace(".csv", ""))
            (d0 / "telemetry.csv").write_bytes(raw)
            n = sum(1 for l in raw.decode("utf-8", "replace").splitlines() if l.startswith("T,"))
            hub.session_dir, hub.note, hub.viewing = d0, d0.name, d0.name
            hub.push("status", hub.snapshot())
            hub.load_csv_frames(d0 / "telemetry.csv")
            hub.push("status", hub.snapshot())
            self._json({"ok": True, "msg": "已导入 %s（%d 帧）" % (fn, n)})
        elif p == "/api/open":
            # 「打开日志」：把一份历史日志读回界面（记录中不允许 ✗，避免与当前进程冲突）
            d0 = telemetry.repo_root() / "logs" / ((d.get("name") or "").strip())
            if not str(d.get("name") or "").strip():
                self._json({"ok": False, "msg": "请选择一份日志，或粘一个绝对路径"})
                return
            if d0.is_absolute() is False and not d0.exists():
                self._json({"ok": False, "msg": "找不到这份日志：%s" % d0})
                return
            if hub.rec is not None:
                self._json({"ok": False, "msg": "正在记录 —— 先点「停止接收」再打开历史日志"})
                return
            if hub.monitoring:
                self._json({"ok": False, "msg": "监测中 —— 先点「停止监测」再打开历史日志"})
                return
            if not (d0 / "telemetry.csv").exists():
                self._json({"ok": False, "msg": "找不到这份日志的 telemetry.csv"})
                return
            # ⚠️ 顺序很关键：**必须先设状态并推给前端，再推帧** ✗
            #    否则前端还不知道处于“浏览历史”模式，会把历史帧全当无关数据丢掉
            #    （实测：表格里只剩后台 BLE 实时帧，看着像“打开失败”✓）
            hub.session_dir = d0
            hub.note = d0.name
            hub.viewing = d0.name
            hub.push("status", hub.snapshot())
            n = hub.load_csv_frames(d0 / "telemetry.csv")
            hub.push("status", hub.snapshot())
            self._json({"ok": True, "msg": "已打开 %s（%d 帧，只读浏览）" % (d0.name, n)})
        elif p == "/api/stopall":
            # 「停止接收」= 停止记录（含自动生成共享摘要）+ **断开蓝牙连接**
            # ⚠️ 2026-10-01 用户要求：按钮叫“停止接收”就该真地停住接收 ✓
            #    （旧版叫“停止并保存”，语义模糊，且连着蓝牙但不记录，容易误解 ✗）
            out = stop_record(hub)
            w = getattr(self.server, "worker", None)
            if w is not None:
                w.stop()
                self.server.worker = None
            self._json({"ok": True, "share": str(out or "")})
        else:
            self._send(404, b"not found", "text/plain")

    def _start_worker(self, device, addr):
        start_worker(self.server, self.hub, device, addr)

    # ---- SSE ----
    def _sse(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.end_headers()

        q = self.hub.sub()
        try:
            self.server.cancel_exit()           # ⭐ 有新页面连上来 ⇒ 取消“关页就退服务”的预约 ✓
            # ⭐ 面板（重新）打开时的清理（2026-10-01 用户报这是 bug ✗）：
            #    只要【当前没在记录】⇒ 之前那批打点是上一轮的残留，清掉 ✓
            #    （正在记录时保留：那是本轮的，刷新页面不应该丢 ✓）
            if self.hub.rec is None:
                self.hub.markers = []
            self._sse_write("hello", self.hub.snapshot())
            last_stat = 0.0
            while True:
                try:
                    kind, payload = q.get(timeout=1.0)
                    self._sse_write(kind, payload)
                except queue.Empty:
                    pass
                now = time.time()
                if now - last_stat >= 1.0:                       # 每秒推一次状态（帧率等）
                    last_stat = now
                    self._sse_write("status", self.hub.snapshot())
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            self.hub.unsub(q)

    def _sse_write(self, event: str, payload):
        data = json.dumps(payload, ensure_ascii=False)
        self.wfile.write(("event: %s\ndata: %s\n\n" % (event, data)).encode("utf-8"))
        self.wfile.flush()


# ============================== main ==============================
def main(argv=None):
    ap = argparse.ArgumentParser(description="蓝牙实时日志（网页界面）")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--device", default="", help="蓝牙名（默认 JDY-69A-BLE / HC-06）")
    ap.add_argument("--addr", default="", help="直接给 MAC，跳过扫描")
    ap.add_argument("--no-browser", action="store_true")
    args = ap.parse_args(argv)

    hub = Hub()
    Handler.hub = hub

    url = "http://127.0.0.1:%d/lpynb" % args.port
    # 端口被占用 ⇒ 说明已经有一个界面在跑：直接把浏览器指过去，不报错 ✓
    # （双击两次 bat 是很常见的情形，不能直接喷错误 ✗）
    try:
        srv = Server(("127.0.0.1", args.port), Handler)
    except OSError:
        print("端口 %d 已被占用（已经有一个界面在运行）⇒ 直接打开浏览器" % args.port)
        webbrowser.open(url)
        return 0
    srv.worker = None
    srv.hub = hub                              # 给 arm_exit 查“还有没有页面连着” ✓

    # ⭐ 2026-10-01 用户要求：**开 bat 就自动连蓝牙**（不再需要“连接蓝牙”按钮 ✓）
    #    没连上就每 5 秒重试，状态在【未连接 / 连接中 / 已连接】间切 ✓
    start_worker(srv, hub, args.device or None, args.addr or None)

    url = "http://127.0.0.1:%d/lpynb" % args.port
    print("=" * 58)
    print("  工创小车 · 蓝牙实时日志（网页版）")
    print("  界面地址：%s" % url)
    print("  连接蓝牙：在页面上点「开始接收」")
    print("  关掉本窗口 = 停止服务（数据已落在 logs/，不会丢）")
    print("=" * 58)
    if not args.no_browser:
        threading.Timer(0.6, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n退出")
    finally:
        srv.shutdown()


if __name__ == "__main__":
    main()
