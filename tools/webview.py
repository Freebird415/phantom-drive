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
import json
import queue
import sys
import threading
import time
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
        self.n_recv = 0                       # 本轮收到的 $T 帧数
        self.n_us = 0
        self.t0 = time.time()
        self.state = "idle"                   # idle / connecting / live / error
        self.msg = "就绪"
        self.session_dir: Path | None = None
        self.rec = None                       # telemetry.Recorder
        self.note = ""
        self.markers: list[dict] = []
        self.last_us = [None, None]           # 左右超声波最近值
        self.last_us_t = 0.0
        self.last_params = {}
        self.events: list[dict] = []
        self.frame_ms = 20.0
        self.drop_us = 0                      # $U 的 -6/-3 次数（驱动拦下，不是故障）
        self.n_bad_us = 0                     # $U 的 -1/-2 次数

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
            "note": self.note, "markers": self.markers[-80:],
            "us": self.last_us, "us_age": (time.time() - self.last_us_t) if self.last_us_t else None,
            "params": self.last_params, "events": self.events[-40:],
            "frame_ms": self.frame_ms, "us_dropped": self.drop_us, "us_bad": self.n_bad_us,
        }

    # ---- 收到一行遥测 ----
    def on_line(self, line: str):
        parsed = telemetry.parse_line(line)
        if not parsed:
            return
        kind, data = parsed

        if self.rec is not None:
            try:
                self.rec.feed(line)
            except Exception:
                pass

        if kind == "T":
            self.n_recv += 1
            self.frames.append({
                "i": int(data["ms"]) // max(1, int(self.frame_ms)),   # 帧号（近似）
                "ms": data["ms"], "gray": "%d%d%d%d%d%d%d%d" % tuple(data["s%d" % k] for k in range(8)),
                "err": data["err"], "pl": data["pwm_l"], "pr": data["pwm_r"], "st": data["state"],
                "dl": data.get("dl", 0), "dr": data.get("dr", 0), "gz": data.get("gz", 0),
            })
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
    """在后台线程跑 asyncio + bleak，把解出来的行喂给 Hub。"""

    def __init__(self, hub: Hub, device: str, addr: str | None = None):
        super().__init__(daemon=True)
        self.hub, self.device, self.addr = hub, device, addr
        self.stopped = threading.Event()

    def stop(self):
        self.stopped.set()

    def run(self):
        try:
            asyncio.run(self._main())
        except Exception as e:                                  # noqa: BLE001
            self.hub.state = "error"
            self.hub.msg = "蓝牙线程异常：%s" % e
            self.hub.push("status", self.hub.snapshot())

    async def _main(self):
        from bleak import BleakClient, BleakScanner

        hub = self.hub
        hub.state = "connecting"
        hub.msg = "扫描 %s ..." % self.device
        hub.push("status", hub.snapshot())

        dev = None
        if self.addr:
            dev = self.addr
        else:
            for nm in ([self.device] if self.device else btlog.KNOWN):
                dev = await BleakScanner.find_device_by_name(nm, timeout=15.0)
                if dev is not None:
                    break
        if dev is None:
            hub.state = "error"
            hub.msg = "没找到模块（确认：① 车上总开关打开 ② 电脑蓝牙已开）"
            hub.push("status", hub.snapshot())
            return

        de = btlog.DeFramer()
        hub.msg = "已连接，接收中 ..."
        hub.state = "live"
        hub.t0 = time.time()
        hub.push("status", hub.snapshot())

        def on_notify(_ch, data: bytes):
            for line in de.feed(bytes(data)):
                hub.on_line(line)
            hub.frame_ms = de.frame_ms

        async with BleakClient(dev, timeout=25.0) as cl:
            await cl.start_notify(btlog.NOTIFY, on_notify)
            while not self.stopped.is_set():
                await asyncio.sleep(0.2)
            try:
                await cl.stop_notify(btlog.NOTIFY)
            except Exception:
                pass
        hub.state = "idle"
        hub.msg = "已断开"
        hub.push("status", hub.snapshot())


# ============================== 录制 / 导出 编排 ==============================
def start_record(hub: Hub, note: str):
    if hub.rec is not None:
        return
    out = telemetry.new_experiment_dir(note or "BLE实时")
    hub.session_dir = out
    hub.note = note or "BLE实时"
    hub.rec = telemetry.Recorder(out, hub.note, port="BLE", baud=0, seconds=0)
    hub.markers = []
    hub.n_recv = 0
    hub.n_us = 0
    hub.t0 = time.time()
    hub.events = []
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
        out = auto_share(hub.session_dir, hub.note)
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

    def _body_json(self) -> dict:
        try:
            n = int(self.headers.get("Content-Length") or 0)
            return json.loads(self.rfile.read(n).decode("utf-8")) if n else {}
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
        elif p == "/api/stream":
            self._sse()
        else:
            self._send(404, b"not found", "text/plain")

    def do_POST(self):
        p = self.path.split("?")[0]
        d = self._body_json()
        hub = self.hub
        if p == "/api/start":
            hub.note = (d.get("note") or "BLE实时").strip()
            if hub.state not in ("live", "connecting"):
                self._start_worker((d.get("device") or "").strip() or None, d.get("addr") or None)
            start_record(hub, hub.note)
            self._json({"ok": True, "session": str(hub.session_dir or "")})
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
        elif p == "/api/discard":
            if hub.rec is not None:
                stop_record(hub, make_share=False)
            d0 = hub.session_dir
            ok = False
            if d0 and d0.exists():
                try:
                    btview._rmtree_force(d0)
                    ok = True
                except Exception as e:                            # noqa: BLE001
                    self._json({"ok": False, "msg": str(e)})
                    return
            hub.session_dir = None
            hub.push("status", hub.snapshot())
            self._json({"ok": ok, "msg": "已删除本次日志" if ok else "没有可删的日志"})
        elif p == "/api/quit":
            # 网页上的「退出服务」：告诉浏览器一声，然后关掉本地服务
            # （因为 bat 用 pythonw 后台启动，没有终端窗口可关 ⇒ 必须留一个退出入口 ✓）
            self._json({"ok": True})
            threading.Timer(0.4, self.server.shutdown).start()
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
        w = getattr(self.server, "worker", None)
        if w is not None:
            w.stop()
        w = BleWorker(self.hub, device or btlog.KNOWN[0], addr)
        self.server.worker = w
        w.start()

    # ---- SSE ----
    def _sse(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.end_headers()

        q = self.hub.sub()
        try:
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

    if args.device or args.addr:                # 给了设备就自动连（否则等页面点「开始接收」）
        Handler._start_worker(Handler, args, args.device or None, args.addr or None)

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
