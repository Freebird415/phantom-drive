# -*- coding: utf-8 -*-
"""把一次跑车日志变成一份【小而可分享】的 Markdown —— 放进仓库的 `共享日志/`

为什么有这个工具（2026-10-01 用户要求）：
    `logs/` 是本机详细数据（CSV 全量，**不入库**）⇒ 组员之间看不到彼此的跑车数据 ✗。
    但整份 CSV 一段就 200KB+，全进仓库会让仓库变胖 ✗。
    ⇒ 折中：**仓库里只放“摘要版”**（Markdown，约 3~6KB）：
        汇总统计 + ASCII 曲线 + 关键帧 + 记录者自己写的结论
    需要细节时，按 md 里写的【本地完整数据】路径找本人要原始 CSV ✓

用法：
    python tools/shared_log.py <日志目录>                       # 目录名自动当标题
    python tools/shared_log.py <目录> --name "A01-提速版" --who 熊雅琦 --commit fd83173
    python tools/shared_log.py <目录> --obs-file 我的结论.txt     # 把结论写进 md

也可以由图型工具「蓝牙日志.bat」的「保存日志…」顺手生成（勾选“同时生成共享版”）。
"""
from __future__ import annotations

import argparse
import csv
import io
import os
import sys
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
SHARED = REPO / "logs"          # 单文件夹方案：共享摘要就放在 logs/ 里（见 logs/README.md）
PREFIX = "共享_"                 # 只有 `共享_*.md` 会被 git 放行入库

BLOCKS = "▁▂▃▄▅▆▇█"
MM_PER_COUNT = 0.115        # 名义值（推定，未精标 —— 见 项目记录/经验与踩坑.md ㊾）
WB_MM = 150.0
DT_S = 1.0 / 33.3           # ⚠️ 仅作兼容保留：真正用的是 rows_dt_ms() 从 ms 列推出来的 DT
                            #（固件 BLE 分频调过 3→2 ⇒ 写死 30ms 会错 1.5 倍 ✗）


def frame_timing(rows):
    """返回 (mult, dt_ms)：一帧代表几个 10ms 采样、相邻两帧真实间隔（毫秒）。

    ⚠️ 背景与历史包袱（2026-10-01 整理，本项目在这里栽过三次）：
      · 固件控制环固定 **100Hz** ⇒ **帧间隔 = 10ms × 发送分频**（恒定不变的关系 ✓）
      · BLE：分频 3 ⇒ 30ms/×3（旧）；分频 2 ⇒ 20ms/×2（2026-10-01 起）
      · 板载快照（插线 DUMP）：逐帧 ⇒ 10ms/×1
      · ❗2026-10-01 **之前** btlog 把 ms 写成“帧号×40”的合成值（不管真实分频）⇒
        那批日志的 ms 恒为 40ms，而其真实分频是 3 ⇒ **看到 ~40ms 就按 ×3 处理** ✓
        （新日志的 ms = 帧号×真实间隔，40ms 只会对应分频 4 —— 本项目没用过）
    ⇒ dt 一律取 mult×10ms（由构定关系推出，不被 ms 合成值带偏）✓
    """
    med = rows_dt_ms_raw(rows)
    if 36.0 <= med <= 44.0:            # 旧的 40ms 合成值 ⇒ 真实是 ×3 / 30ms
        return 3.0, 30.0
    m = max(1.0, min(10.0, round(med / 10.0)))
    return m, m * 10.0


def frame_mult(rows) -> float:
    """一帧代表多少个 10ms 采样（= 固件的发送分频）—— 详见 frame_timing()。"""
    return frame_timing(rows)[0]


def rows_dt_ms(rows) -> float:
    """相邻两帧的真实时间差（ms）—— 详见 frame_timing()。"""
    return frame_timing(rows)[1]


def rows_dt_ms_raw(rows) -> float:
    """只看 ms 列的中位数差值（不做任何修正）；无 ms 时退 30ms。"""
    dif = []
    for i in range(1, min(len(rows), 400)):
        try:
            d = int(rows[i]["ms"]) - int(rows[i - 1]["ms"])
        except (KeyError, ValueError, TypeError):
            continue
        if d > 0:
            dif.append(d)
    if not dif:
        return 30.0
    dif.sort()
    return float(dif[len(dif) // 2])


def spark(vals, width=96, lo=None, hi=None):
    """把一串数字压成一行字符柱状图（▁▂▃▄▅▆▇█）。"""
    if not vals:
        return ""
    step = max(1, len(vals) // width)
    ds = [sum(vals[i:i+step]) / len(vals[i:i+step]) for i in range(0, len(vals), step)][:width]
    if lo is None:
        lo = min(ds)
    if hi is None:
        hi = max(ds)
    if hi - lo < 1e-9:
        return BLOCKS[0] * len(ds)
    return "".join(BLOCKS[max(0, min(7, int((v - lo) / (hi - lo) * 7.999)))] for v in ds)


def load(dirpath: Path):
    p = dirpath / "telemetry.csv"
    if not p.exists():
        sys.exit("找不到 %s —— 请指向一次日志/片段的目录" % p)
    rows = [r for r in csv.DictReader(io.open(p, encoding="utf-8")) if r.get("kind") == "T"]
    return rows


def f(v, d=0.0):
    try:
        return float(v)
    except (TypeError, ValueError):
        return d


def summarize(rows):
    MULT = frame_mult(rows)               # 一帧代表几个 10ms 采样（BLE 分频 / 快照）
    DT = rows_dt_ms(rows) / 1000.0        # 每帧真实时间步长（秒）—— 不再写死 30ms ✗
    err, pl, pr, spd, gy, enc, gray = [], [], [], [], [], [], []
    for r in rows:
        dl, dr = f(r.get("dl")), f(r.get("dr"))
        g = f(r.get("gz"))
        ml, mr = dl * MM_PER_COUNT, dr * MM_PER_COUNT
        err.append(f(r.get("err")))
        pl.append(f(r.get("pwm_l")))
        pr.append(f(r.get("pwm_r")))
        # ⚠️ 一帧代表的路程 = (mm_l+mm_r)/2 × MULT
        #    BLE 每帧只是 10ms 增量（MULT = 分频）；板载快照本来就是逐帧（MULT=1）
        spd.append((ml + mr) / 2.0 * MULT / DT)
        gy.append(g)
        enc.append(((mr - ml) / WB_MM) * 57.2958 / DT)
    n = max(1, len(rows))
    mov = [i for i, r in enumerate(rows) if abs(f(r.get("dl"))) + abs(f(r.get("dr"))) > 0
           or f(r.get("pwm_l")) or f(r.get("pwm_r"))]
    st = {}
    for r in rows:
        st[r.get("state", "?")] = st.get(r.get("state", "?"), 0) + 1
    # 与 btview 的 note.md 口径统一：每帧位移 = spd × DT；有符号累加（前进为正）
    dist = sum(spd) * DT
    ae = [abs(e) for e in err]
    yaw_g = sum(gy) * DT
    yaw_e = sum(enc) * DT
    return {
        "frames": len(rows), "dur": len(rows) * DT, "moving": len(mov), "mult": MULT,
        "dist_mm": dist, "v_mean": sum(abs(v) for v in spd) / n,
        "v_max": max([abs(v) for v in spd], default=0.0),
        "err_mean": sum(ae) / n, "err_max": max(ae, default=0.0),
        "err_le1": 100.0 * sum(1 for e in ae if e <= 1.0) / n,
        "yaw_gyro": yaw_g, "yaw_enc": yaw_e,
        "gyro_nz": sum(1 for g in gy if g != 0.0),
        "states": st, "err": err, "spd": spd, "gy": gy, "pl": pl, "pr": pr,
    }


def build_md(dirpath: Path, name: str, who: str, commit: str, note: str,
             obs: str, rows) -> str:
    s = summarize(rows)
    # 关键帧（若目录里有 markers.txt）
    keys = []
    mk = dirpath / "markers.txt"
    if mk.exists():
        keys = [l.strip() for l in mk.read_text(encoding="utf-8").splitlines() if l.strip()]
    now = datetime.now().strftime("%Y-%m-%d %H:%M")
    states = " ".join("%s×%d" % (k, v) for k, v in sorted(s["states"].items()))
    L = []
    A = L.append
    A("# %s" % (name or dirpath.name))
    A("")
    A("| 项 | 值 |")
    A("|---|---|")
    A("| 记录者 | %s |" % (who or "（待填）"))
    A("| 时间 | %s |" % now)
    if commit:
        A("| 固件提交 | `%s` |" % commit)
    A("| 备注 | %s |" % (note or "—"))
    # ⚠️ 这一句以前写死“约 33 帧/s”✗ —— 固件 BLE 分频 2026-10-01 从 3 改成 2（50fps）
    #    ⇒ 写死就过期了（跟“状态白名单”“ms=40”同一类坑）⇒ 改成从数据自身推 ✓
    _m = s.get("mult", 3.0) if isinstance(s, dict) else 3.0
    _dt = _m * 10.0
    if abs(_m - 1.0) < 0.01:
        A("| 数据来源 | 板载快照（100Hz 逐帧；每帧 = 1 个 10ms 采样）|")
    else:
        A("| 数据来源 | 无线 BLE（约 %.0f 帧/s，帧间隔 %.0fms；每帧的 dL/dR 只是 10ms 增量 ⇒ 汇总已 ×%g）|"
          % (1000.0 / _dt, _dt, _m))
    A("| **本地完整数据** | `%s`（**未入库**，要细节找本人）|"
      % str(dirpath).replace("\\", "/"))
    A("")
    A("## 一、结论（记录者本人填 —— 这是这份日志最值钱的部分）")
    A("")
    if obs.strip():
        A(obs.strip())
    else:
        A("> 现象：")
        A("> 这版相比上一版：")
        A("> 仍存在的问题：")
        A("> 下一步想试：")
    A("")
    A("## 二、自动汇总（由 `tools/shared_log.py` 生成）")
    A("")
    A("| 指标 | 值 |")
    A("|---|---|")
    A("| 帧数 / 其中在动 | %d / %d |" % (s["frames"], s["moving"]))
    A("| 名义时长 | %.1f s |" % s["dur"])
    A("| 里程（差速推算，名义尺度） | %.0f mm |" % s["dist_mm"])
    A("| 速度：均值 / 峰值 | %.1f / %.1f mm/s |" % (s["v_mean"], s["v_max"]))
    A("| 循迹偏差 \\|err\\|：均值 / 最大 / ≤1 占比 | %.2f / %.1f / %.0f%% |"
      % (s["err_mean"], s["err_max"], s["err_le1"]))
    A("| 累计转角：陀螺仪 / 编码器差速 | %+.1f° / %+.1f° |" % (s["yaw_gyro"], s["yaw_enc"]))
    A("| gz 非零帧（陀螺仪有没有在报数） | %d / %d |" % (s["gyro_nz"], s["frames"]))
    A("| state 分布 | %s |" % states)
    A("")
    A("## 三、曲线速览（每格 ≈ %d 帧，纵轴各自归一化）" % max(1, s["frames"] // 96))
    A("")
    A("```")
    A("偏差 err   " + spark(s["err"]))
    A("左轮 pwm   " + spark(s["pl"]))
    A("速度 mm/s  " + spark(s["spd"]))
    A("陀螺仪 °/s " + spark(s["gy"]))
    A("```")
    A("")
    A("## 四、关键帧（记录者按空格打的点）")
    A("")
    if keys:
        for k in keys[:40]:
            A("- " + k)
    else:
        A("- （这份日志的目录里没有 `markers.txt`：导出片段时会带过来；原始日志的 `markers.txt` 在母目录）")
    A("")
    A("## 五、读这份数据要知道的三件事")
    A("")
    A("1. **每帧的时间含义**：本份来自 %s ⇒ %s"
      % ("板载快照（100Hz 逐帧）" if abs(s.get("mult", 3.0) - 1.0) < 0.01
         else "无线 BLE（帧间隔 %.0fms）" % (s.get("mult", 3.0) * 10.0),
         "一帧就是一个 10ms 采样，**直接累加即可**" if abs(s.get("mult", 3.0) - 1.0) < 0.01
         else "一帧只是 10ms 的增量（固件每 %g 个采样才发 1 帧）⇒ 自己按行累加要 **×%g**；上面的汇总是对的 ✓"
              % (s.get("mult", 3.0), s.get("mult", 3.0))))
    A("2. `state`：0=丢线/待机 · 1=循迹 · 5=起步找线 · 4=精停（2/3 留给避障）")
    A("3. mm/s 用**名义尺度** %.3f mm/计数（推定值，未精标，误差量级 ±20~40%%）——比趋势可靠，当绝对尺要小心" % MM_PER_COUNT)
    A("")
    A("> 想深入分析：让记录者把 `%s` 整个目录（或其中 `telemetry.csv`）发给你，"
      "用 `python tools/plot.py <目录>` 出图，或直接读它的 `derived.csv`。" % dirpath.name)
    A("")
    return "\n".join(L)


def main() -> int:
    try:
        sys.stdout.reconfigure(errors="replace")   # 控制台是 GBK：不可编码字符降级，别让脚本崩
    except Exception:
        pass
    ap = argparse.ArgumentParser(description="把日志目录生成一份可入库的共享版 md")
    ap.add_argument("dir", help="日志或片段目录（含 telemetry.csv）")
    ap.add_argument("--name", default=None, help="标题（默认用目录名；建议 日期_任务号_名字_版本）")
    ap.add_argument("--who", default="", help="记录者")
    ap.add_argument("--commit", default="", help="对应固件提交（短 hash）")
    ap.add_argument("--note", default="", help="一句话备注")
    ap.add_argument("--obs-file", default=None, help="把这份文件的文本放进“结论”一节")
    ap.add_argument("--out", default=None, help="输出路径（默认 共享日志/<name>.md）")
    ap.add_argument("--stdout", action="store_true", help="只打印，不写文件")
    a = ap.parse_args()

    d = Path(a.dir)
    rows = load(d)
    if not rows:
        sys.exit("这个目录里没有数据帧")
    obs = Path(a.obs_file).read_text(encoding="utf-8") if a.obs_file else ""
    md = build_md(d, a.name or d.name, a.who, a.commit, a.note, obs, rows)
    if a.stdout:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        print(md)
        return 0
    out = Path(a.out) if a.out else (SHARED / ("%s%s.md" % (PREFIX, a.name or d.name)))
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(md, encoding="utf-8")
    print("已生成共享版：%s（%.1f KB）" % (out, out.stat().st_size / 1024.0))
    print("提醒：① 在“结论”一节写上你自己的判断 ② 原始 CSV 不用入库（按 md 里的路径留本地）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
