"""遥测协议解析 + 实验档案（P01 反馈链路）

协议规范见 docs/遥测协议.md —— 改协议时两边一起改。

被 tools/run.py（采集存档）和 tools/plot.py（画图）共用。
"""
from __future__ import annotations

import csv
import json
import subprocess
from datetime import datetime
from pathlib import Path

# ---------------------------------------------------------------- 协议
CSV_COLUMNS = [
    "kind", "ms",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "err", "pwm_l", "pwm_r", "state",
    "kp", "ki", "kd", "base_speed",
    "code", "note",
    "l_mm", "r_mm",
]


def parse_line(line: str):
    """解析一行遥测。返回 (kind, dict) 或 None。

    $T,ms,s0..s7,err,pwm_l,pwm_r,state           14 段
    $P,ms,kp,ki,kd,base_speed                     6 段
    $E,ms,code,note                               4 段
    $U,ms,l_mm,r_mm                               4 段（超声波，-1=无回波）
    """
    line = line.strip()
    if not line.startswith("$"):
        return None
    parts = line.split(",")
    try:
        if parts[0] == "$T" and len(parts) == 14:
            return "T", {
                "ms": int(parts[1]),
                **{f"s{i}": int(parts[2 + i]) for i in range(8)},
                "err": float(parts[10]),
                "pwm_l": int(parts[11]),
                "pwm_r": int(parts[12]),
                "state": int(parts[13]),
            }
        if parts[0] == "$P" and len(parts) == 6:
            return "P", {
                "ms": int(parts[1]),
                "kp": float(parts[2]),
                "ki": float(parts[3]),
                "kd": float(parts[4]),
                "base_speed": int(parts[5]),
            }
        if parts[0] == "$E" and len(parts) == 4:
            return "E", {"ms": int(parts[1]), "code": int(parts[2]), "note": parts[3]}
        if parts[0] == "$U" and len(parts) == 4:
            return "U", {"ms": int(parts[1]), "l_mm": int(parts[2]), "r_mm": int(parts[3])}
    except ValueError:
        return None
    return None


# ---------------------------------------------------------------- 实验档案
def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def git_info() -> dict:
    """记录当前代码版本——实验能否复现取决于它"""
    def run(*args):
        try:
            out = subprocess.run(["git", *args], cwd=repo_root(),
                                 capture_output=True, text=True, timeout=10)
            return out.stdout.strip()
        except Exception:
            return ""

    commit = run("rev-parse", "--short", "HEAD")
    dirty = bool(run("status", "--porcelain", "firmware"))
    return {"commit": commit, "firmware_dirty": dirty}


def new_experiment_dir(note: str) -> Path:
    """logs/2026-09-13_1530_过弯抖/"""
    tag = "".join(c for c in note.strip() if c not in '\\/:*?"<>|').strip() or "未命名"
    stamp = datetime.now().strftime("%Y-%m-%d_%H%M")
    d = repo_root() / "logs" / f"{stamp}_{tag[:20]}"
    d.mkdir(parents=True, exist_ok=True)
    return d


class Recorder:
    """采集过程中写 CSV；结束时写 meta.json / note.md"""

    def __init__(self, out_dir: Path, note: str, port: str, baud: int, seconds: float):
        self.dir = out_dir
        self.note = note
        self.counts = {"T": 0, "P": 0, "E": 0, "U": 0, "raw": 0}
        self.events: list[dict] = []
        self.params: list[dict] = []
        self.raw_lines: list[str] = []
        self._csv = (out_dir / "telemetry.csv").open("w", newline="", encoding="utf-8")
        self._writer = csv.DictWriter(self._csv, fieldnames=CSV_COLUMNS, extrasaction="ignore")
        self._writer.writeheader()
        self.meta = {
            "note": note, "port": port, "baud": baud, "seconds": seconds,
            "started": datetime.now().isoformat(timespec="seconds"),
            **git_info(),
        }

    def feed(self, line: str) -> bool:
        """喂入一行串口数据；识别到遥测帧返回 True"""
        parsed = parse_line(line)
        if not parsed:
            if line.strip():
                self.counts["raw"] += 1
                if len(self.raw_lines) < 50:
                    self.raw_lines.append(line.strip())
            return False
        kind, data = parsed
        self.counts[kind] += 1
        row = {"kind": kind, **data}
        self._writer.writerow(row)
        if kind == "E":
            self.events.append(data)
        if kind == "P":
            self.params.append(data)
        return True

    def close(self):
        self._csv.close()
        self.meta["counts"] = self.counts
        self.meta["finished"] = datetime.now().isoformat(timespec="seconds")
        (self.dir / "meta.json").write_text(
            json.dumps(self.meta, ensure_ascii=False, indent=2), encoding="utf-8")

        lines = [f"# {self.note}", "",
                 f"- 时间：{self.meta['started']}",
                 f"- 端口：{self.meta['port']} @ {self.meta['baud']}",
                 f"- 时长：{self.meta['seconds']} 秒",
                 f"- 代码版本：`{self.meta['commit']}`"
                 + ("（**有未提交改动**）" if self.meta["firmware_dirty"] else ""),
                 f"- 收到：{self.counts['T']} 帧遥测 / {self.counts['P']} 帧参数 / "
                 f"{self.counts['E']} 个事件 / {self.counts['U']} 帧超声波", ""]
        if self.events:
            lines += ["## 事件", ""]
            lines += [f"- {e['ms'] / 1000:.2f}s  事件 {e['code']}：{e['note']}" for e in self.events]
            lines.append("")
        if self.raw_lines:
            lines += ["## 非遥测输出（前几条）", "", "```"]
            lines += self.raw_lines[:10]
            lines += ["```", ""]
        (self.dir / "note.md").write_text("\n".join(lines), encoding="utf-8")


def load_csv(path: Path) -> list[dict]:
    """读回日志（plot.py 用），自动把空字段转成 None"""
    rows = []
    with path.open(encoding="utf-8") as f:
        for r in csv.DictReader(f):
            rows.append({k: (v if v != "" else None) for k, v in r.items()})
    return rows
