"""从日志重建【小车实际轨迹 = 轨道形状】（航位推算）

用法（仓库根目录）：
    python tools/track.py logs/xxx                  # 目录或 CSV
    python tools/track.py logs/xxx --mm-per-count 0.0764 --wheelbase 150

原理（左右轮真实速度 ⇒ 航位推算）：
    v_L = dl × mm_per_count / dt        # 左轮真实速度 mm/s
    v_R = dr × mm_per_count / dt        # 右轮
    v   = (v_L + v_R) / 2               # 车心速度 ⇒ 积分得【走过的距离】
    ω   = (v_R − v_L) / 轮距            # 角速度   ⇒ 积分得【车头朝向】
    x += v·cos(θ)·dt ;  y += v·sin(θ)·dt ;  θ += ω·dt
    ⇒ (x, y) 连起来就是小车实际走的轨迹 = 轨道形状 ✓

⚠️ 需要日志里有 dl/dr（固件 2026-10-01 起才加）⇒ 旧日志会报错提示。
⚠️ mm_per_count（每计数多少毫米）没标定时用推定值，**轨迹的长度比例可能不准，
   但形状是对的**（同一份日志里各段长度的【相对】关系正确）✓
"""
from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent))
import telemetry  # noqa: E402

# 推定值（没标定时的兜底）：JGB37-520 12V 330RPM + 7cm 轮
#   实测手转 ~20RPM 时 ~960 计数/s ⇒ 每转 ≈ 2880 计数；轮周长 π×70 ≈ 220mm
DEFAULT_MM_PER_COUNT = 220.0 / 2880.0     # ≈ 0.0764
DEFAULT_WHEELBASE_MM = 150.0              # 两驱动轮中心距（最好量一下）
DEFAULT_DT_S = 0.0087                     # 实测帧间隔 ≈ 8.7ms（不是标称 10ms；会从 ms 自动算）


def resolve_csv(target: Path) -> Path:
    if target.is_dir():
        return target / "telemetry.csv"
    return target


def main():
    ap = argparse.ArgumentParser(description="从日志重建轨道形状（航位推算）")
    ap.add_argument("path", help="logs/xxx 目录或 telemetry.csv")
    ap.add_argument("--mm-per-count", type=float, default=DEFAULT_MM_PER_COUNT,
                    help=f"每计数多少毫米（默认推定 {DEFAULT_MM_PER_COUNT:.4f}，精确需标定）")
    ap.add_argument("--wheelbase", type=float, default=DEFAULT_WHEELBASE_MM,
                    help=f"两驱动轮中心距 mm（默认 {DEFAULT_WHEELBASE_MM:g}）")
    ap.add_argument("--out", help="输出图片路径（默认与 CSV 同目录的 track.png）")
    ap.add_argument("--use-gyro", action="store_true",
                    help="用陀螺仪积分出的朝向（比轮式里程计准），并与轮式结果对比报告")
    args = ap.parse_args()

    csv_path = resolve_csv(Path(args.path))
    if not csv_path.exists():
        sys.exit(f"找不到：{csv_path}")
    rows = [r for r in telemetry.load_csv(csv_path) if r.get("kind") == "T"]
    rows = [r for r in rows if r.get("dl") not in (None, "")]
    if not rows:
        sys.exit("日志里没有 dl/dr 字段（旧格式）⇒ 无法重建轨迹。\n"
                 "请用 2026-10-01 之后的固件重新跑一趟（板载快照会带 dl/dr）✓")

    # ---- 航位推算 ----
    use_gyro = args.use_gyro and any(r.get("gz") not in (None, "") for r in rows)
    if args.use_gyro and not use_gyro:
        print("⚠️ 日志里没有 gz（陀螺仪）字段 ⇒ 退回轮式里程计")
    th_wheel = 0.0      # 轮式里程计积出的朝向（用于对比）
    x = y = 0.0
    th = 0.0
    xs, ys, ths, vs = [0.0], [0.0], [0.0], [0.0]
    dist = 0.0
    prev_ms = None
    for r in rows:
        ms = float(r["ms"])
        dt = (ms - prev_ms) / 1000.0 if prev_ms is not None else DEFAULT_DT_S
        prev_ms = ms
        if dt <= 0:
            continue
        vL = float(r["dl"]) * args.mm_per_count / dt
        vR = float(r["dr"]) * args.mm_per_count / dt
        v = (vL + vR) / 2.0
        w = (vR - vL) / args.wheelbase
        th_wheel += w * dt
        if use_gyro:
            th += math.radians(float(r["gz"])) * dt     # 陀螺仪：直接积分角速度
        else:
            th += w * dt
        x += v * math.cos(th) * dt
        y += v * math.sin(th) * dt
        dist += abs(v) * dt
        xs.append(x); ys.append(y); ths.append(th); vs.append(v)

    print(f"帧数 {len(rows)}｜轨迹总长 ≈ {dist:.0f} mm｜净位移 ≈ {math.hypot(x, y):.0f} mm")
    print(f"总转角 ≈ {math.degrees(th):+.0f}°｜平均速度 ≈ {sum(abs(v) for v in vs)/max(1,len(vs)):.0f} mm/s")
    print(f"（mm/计数 = {args.mm_per_count:.4f}，轮距 = {args.wheelbase:g}mm"
          f"{'  ← 推定值' if abs(args.mm_per_count - DEFAULT_MM_PER_COUNT) < 1e-9 else ''}）")
    if use_gyro:
        print(f"★ 用陀螺仪积分朝向；与轮式里程计相比：陀螺 {math.degrees(th):+.0f}° "
              f"vs 轮式 {math.degrees(th_wheel):+.0f}° "
              f"⇒ 差 {abs(math.degrees(th - th_wheel)):.0f}°")
        print("   差太大 ⇒ 说明轮子有打滑/轮距值不对/里程计比例不准 ✓ 这正是拿来交叉验证的意义")

    # ---- 出图 ----
    fig, ax = plt.subplots(figsize=(7, 7))
    ax.plot(xs, ys, "-", color="#1f77b4", lw=1.6)
    ax.plot(xs[0], ys[0], "o", color="green", ms=9, label="起点")
    ax.plot(xs[-1], ys[-1], "s", color="red", ms=9, label="终点")
    # 每隔若干点画个箭头表示朝向
    step = max(1, len(xs) // 18)
    for i in range(0, len(xs), step):
        ax.arrow(xs[i], ys[i], 12 * math.cos(ths[i]), 12 * math.sin(ths[i]),
                 head_width=6, color="#888", length_includes_head=True, alpha=0.7)
    ax.set_aspect("equal", adjustable="datalim")
    ax.grid(alpha=0.3)
    ax.set_xlabel("x / mm"); ax.set_ylabel("y / mm")
    ax.set_title("小车实际轨迹（航位推算）—— 即在赛道上的形状")
    ax.legend(loc="best")
    out = Path(args.out) if args.out else csv_path.with_name("track.png")
    fig.savefig(out, dpi=110, bbox_inches="tight")
    print("已出图：", out)


if __name__ == "__main__":
    main()
