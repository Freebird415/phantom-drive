# -*- coding: utf-8 -*-
"""算法版本管理 —— 把 `firmware/src/app/app_trace.c` 的各版本分文件夹存档，方便对比/回退。

为什么需要（用户 2026-10-01 提出）：
    git 能回溯，但“这一版是哪一版、和上一版差在哪、跑的成绩如何”翻 git 很费劲 ✗。
    ⇒ 每个算法版本存一个文件夹（含当时的 app_trace.c + 说明.md），配一张索引表，
      上车实测时按版本记录日志（logs/共享_*.md），对比一目了然 ✓

用法（在仓库根目录）：
    python tools/algo_version.py list                  # 列出所有版本（含 git 溯源）
    python tools/algo_version.py save "v4_降速保稳"      # 把当前 app_trace.c 存成新版本
    python tools/algo_version.py use v2                # 切到某版本（会先把当前自动存为 _当前快照）
    python tools/algo_version.py diff v2               # 当前 vs 某版本的差异摘要

⚠️ 权威文件永远是 `firmware/src/app/app_trace.c`（编译只看它）；
   本目录只是**存档与对比**用 —— 切完记得重新编译烧录 ✓
"""
from __future__ import annotations

import argparse
import io
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
LIVE = REPO / "firmware" / "src" / "app" / "app_trace.c"
VERDIR = REPO / "firmware" / "算法版本"
KEEP = Path("_当前快照")          # 切换前自动备份的位置


def versions():
    if not VERDIR.exists():
        return []
    return sorted([d for d in VERDIR.iterdir() if d.is_dir()], key=lambda p: p.name)


def git_last(path: Path) -> str:
    try:
        out = subprocess.run(["git", "log", "-1", "--format=%h %ad %an", "--date=short", "--", str(path)],
                             cwd=REPO, capture_output=True, text=True, encoding="utf-8").stdout.strip()
        return out or "（未提交）"
    except Exception:
        return "（取不到）"


def cmd_list(_a):
    print("=== 算法版本（存档在 firmware/算法版本/）===")
    for d in versions():
        f = d / "app_trace.c"
        note = d / "说明.md"
        first = ""
        if note.exists():
            for line in io.open(note, encoding="utf-8").read().splitlines():
                if line.startswith("> ") and len(line) > 3:
                    first = line[2:60]
                    break
        n = len(io.open(f, encoding="utf-8").read().splitlines()) if f.exists() else 0
        print("  %-52s %4d 行" % (d.name, n))
        if first:
            print("       └ %s" % first)
    print("")
    print("当前生效（编译用的）：%s" % LIVE.relative_to(REPO))
    print("  git 溯源：%s" % git_last(LIVE))
    print("")
    print("切版本：python tools/algo_version.py use <名字前缀>   ｜ 存新版：... save \"v4_xxx\"")


def _snapshot_live(tag: str = ""):
    if not LIVE.exists():
        return None
    d = VERDIR / KEEP
    d.mkdir(parents=True, exist_ok=True)
    out = d / ("app_trace_%s.c" % (tag or datetime.now().strftime("%m%d_%H%M")))
    shutil.copy2(str(LIVE), str(out))
    return out


def cmd_save(a):
    name = a.name
    d = VERDIR / name
    if d.exists():
        sys.exit("已存在同名版本：%s（换个名字或先删掉）" % d)
    d.mkdir(parents=True)
    shutil.copy2(str(LIVE), str(d / "app_trace.c"))
    (d / "说明.md").write_text(
        "# %s\n\n- 存档时间：%s\n- 来源：`firmware/src/app/app_trace.c`（当时 git：%s）\n\n"
        "> 一句话说明这一版改了什么 / 为什么\n\n## 改动\n\n- \n\n## 实测结果\n\n- 日志：`logs/共享_....md`\n- 结论：\n"
        % (name, datetime.now().strftime("%Y-%m-%d %H:%M"), git_last(LIVE)), encoding="utf-8")
    print("已存档：%s（记得补 说明.md 与实测结果）" % d.relative_to(REPO))


def _find(prefix: str):
    for d in versions():
        if d.name.startswith(prefix) or prefix in d.name:
            return d
    return None


def cmd_use(a):
    d = _find(a.name)
    if d is None:
        sys.exit("找不到版本：%s（先 list 看看）" % a.name)
    src = d / "app_trace.c"
    if not src.exists():
        sys.exit("%s 里没有 app_trace.c" % d)
    tag = d.name.split("_")[0]
    saved = _snapshot_live("切换前_" + tag)
    shutil.copy2(str(src), str(LIVE))
    print("已切到：%s" % d.name)
    if saved:
        print("  （切换前的文件已备份到 %s）" % saved.relative_to(REPO))
    print("  ⚠️ 记得重新编译烧录：pio run -d firmware -t upload")


def cmd_diff(a):
    d = _find(a.name)
    if d is None:
        sys.exit("找不到版本：%s" % a.name)
    out = subprocess.run(["git", "diff", "--no-index", "--stat", str(d / "app_trace.c"), str(LIVE)],
                         cwd=REPO, capture_output=True, text=True, encoding="utf-8").stdout
    print("=== 版本 %s  vs  当前 %s ===" % (d.name, LIVE.name))
    print(out or "（无差异）")
    print("提示：要看完整差异去掉 --stat，或用 VS Code 对比两个文件 ✓")


def main() -> int:
    try:
        sys.stdout.reconfigure(errors="replace")
    except Exception:
        pass
    ap = argparse.ArgumentParser(description="算法版本管理")
    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("list").set_defaults(func=cmd_list)
    p1 = sub.add_parser("save"); p1.add_argument("name"); p1.set_defaults(func=cmd_save)
    p2 = sub.add_parser("use");  p2.add_argument("name"); p2.set_defaults(func=cmd_use)
    p3 = sub.add_parser("diff"); p3.add_argument("name"); p3.set_defaults(func=cmd_diff)
    a = ap.parse_args()
    if not getattr(a, "func", None):
        return cmd_list(a) or 0
    a.func(a)
    return 0


if __name__ == "__main__":
    sys.exit(main())
