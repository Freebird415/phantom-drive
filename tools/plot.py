"""把实验日志画成图（P01）

用法：
    python tools/plot.py logs/2026-09-13_1530_过弯抖        # 给目录
    python tools/plot.py logs/xxx/telemetry.csv             # 或直接给 CSV

输出：同目录下 plot.png（传感器位置 / 偏差 err / 左右 PWM，事件用虚线标出）
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import telemetry  # noqa: E402

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "SimHei", "DejaVu Sans"]
plt.rcParams["axes.unicode_minus"] = False


def resolve_csv(target: Path) -> Path:
    if target.is_dir():
        return target / "telemetry.csv"
    return target


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    target = Path(sys.argv[1]).resolve()
    csv_path = resolve_csv(target)
    if not csv_path.exists():
        sys.exit(f"找不到日志文件：{csv_path}")

    rows = telemetry.load_csv(csv_path)
    t_frames = [r for r in rows if r["kind"] == "T"]
    u_frames = [r for r in rows if r["kind"] == "U"]
    events = [r for r in rows if r["kind"] == "E"]
    params = [r for r in rows if r["kind"] == "P"]

    if not t_frames and not u_frames:
        sys.exit("日志里既没有 $T 也没有 $U 帧，画不了图。先确认采集时是否收到数据。")

    t = [int(r["ms"]) / 1000 for r in t_frames] or [0.0]
    err = [float(r["err"]) for r in t_frames] or [0.0]
    pwm_l = [int(r["pwm_l"]) for r in t_frames] or [0]
    pwm_r = [int(r["pwm_r"]) for r in t_frames] or [0]
    sensors = [[int(r[f"s{i}"]) for r in t_frames] for i in range(8)]

    n_panels = 4 if u_frames else 3
    fig, axes = plt.subplots(n_panels, 1, figsize=(11, 2.6 * n_panels), sharex=True)

    # ① 八路传感器：黑色=压线，一眼看出线走到了哪一路
    if t_frames:
        axes[0].imshow(sensors, aspect="auto", cmap="Greys",
                       interpolation="nearest",
                       extent=[t[0], t[-1], 7.5, -0.5])
    axes[0].set_ylabel("传感器 0-7")
    axes[0].set_title("循迹传感器（黑=压线）")

    # ② 偏差
    axes[1].plot(t, err, color="#1f77b4", lw=1.2)
    axes[1].axhline(0, color="gray", lw=0.6, ls=":")
    axes[1].set_ylabel("err")
    axes[1].set_title("循迹偏差")

    # ③ 左右 PWM
    axes[2].plot(t, pwm_l, label="pwm_l", color="#d62728", lw=1.2)
    axes[2].plot(t, pwm_r, label="pwm_r", color="#2ca02c", lw=1.2)
    axes[2].set_ylabel("PWM")
    axes[2].set_xlabel("时间 (s)")
    axes[2].set_title("左右轮 PWM")
    axes[2].legend(loc="upper right", fontsize=8)

    # ④ 超声波（有 $U 帧才画）：-1 = 无回波，断开显示（不连成假线）
    if u_frames:
        nan = float("nan")
        tu = [int(r["ms"]) / 1000 for r in u_frames]
        lm = [int(r["l_mm"]) if int(r["l_mm"]) >= 0 else nan for r in u_frames]
        rm = [int(r["r_mm"]) if int(r["r_mm"]) >= 0 else nan for r in u_frames]
        axes[3].plot(tu, lm, label="左 PA4/PA5", color="#1f77b4", lw=1.2)
        axes[3].plot(tu, rm, label="右 PA1/PB5", color="#d62728", lw=1.2)
        axes[3].set_ylabel("mm")
        axes[3].set_xlabel("时间 (s)")
        axes[3].set_title("超声波测距（-1 = 无回波，已断开）")
        axes[3].legend(loc="upper right", fontsize=8)

    # 事件：竖虚线 + 标注
    for e in events:
        te = int(e["ms"]) / 1000
        for ax in axes:
            ax.axvline(te, color="orange", ls="--", lw=0.8)
        axes[0].text(te, -0.9, e["note"], color="orange", fontsize=7,
                     rotation=90, va="bottom", ha="center")

    # 参数信息放标题
    if params:
        p = params[0]
        fig.suptitle(
            f"kp={p['kp']} ki={p['ki']} kd={p['kd']} base_speed={p['base_speed']}"
            + (f"（后续参数变更 {len(params) - 1} 次）" if len(params) > 1 else ""),
            fontsize=10)

    fig.tight_layout(rect=[0, 0, 1, 0.96])
    out = csv_path.with_name("plot.png")
    fig.savefig(out, dpi=120)
    print(f"已出图：{out}")


if __name__ == "__main__":
    main()
