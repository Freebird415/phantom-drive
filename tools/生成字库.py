#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""按"字表"生成 OLED 子集字库 → firmware/src/drv/font_subset.h

为什么不能整库：一个完整中文字库几百 KB~几 MB，而 STM32F103C8T6 只有 **64KB Flash**
（现在固件已用 ~30KB）。所以只生成"项目实际会用到的那几个字"，每个 16×16 字模 32 字节，
32 个字也才 1KB。

字表来源：本文件顶部的 ASCII_RANGE 与 CN_CHARS。
**新增要显示的字 → 加进 CN_CHARS → 重跑本脚本 → 重新编译**（否则那个字显示不出来，
这是"中文显示不出来"最常见的原因）。

用法：  python tools/生成字库.py
输出：  firmware/src/drv/font_subset.h   （自动生成，不要手改）
"""
from __future__ import annotations

from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "firmware" / "src" / "drv" / "font_subset.h"

# ---------------------------------------------------------------- 字表（要显示什么就列什么）
ASCII_RANGE = (0x20, 0x7E)      # 可打印 ASCII：95 个
                                #  · 6×8   （小字，列优先 1 字节/列）
                                #  · 12×16 （大字/仪表盘，列优先 2 字节/列 = 24B/字）
CN_CHARS = (
    "状态速度偏差左右精停循迹避障回线正常错误测试中文显示"
)                # 加字规则：改这里 → 重跑本脚本 → 重新编译                # 2026-10-01 精简：只留实际用到的（省 Flash，给 12×16 ASCII 腾地方）


CJK_FONTS = [r"C:\Windows\Fonts\simhei.ttf", r"C:\Windows\Fonts\msyh.ttc", r"C:\Windows\Fonts\simsun.ttc"]
BIG_CHARS = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789<>-+/.:%"
# ↑ 12×16 大字只生成这些（省 Flash）。要显示新字符 → 加进这里 → 重跑脚本 → 重编译

ASCII_FONTS = [r"C:\Windows\Fonts\consola.ttf", r"C:\Windows\Fonts\cour.ttf", r"C:\Windows\Fonts\arial.ttf"]


def pick(fonts):
    for f in fonts:
        if Path(f).exists():
            return f
    raise SystemExit(f"找不到可用字体：{fonts}")


def bitmap(ch: str, w: int, h: int, font_path: str, size: int):
    """把一个字符渲染成 w×h 黑白位图，返回 w 列 × ceil(h/8) 字节（列优先、bit0=顶行）"""
    from PIL import Image, ImageDraw, ImageFont
    img = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(font_path, size)
    bb = d.textbbox((0, 0), ch, font=f)
    x = (w - (bb[2] - bb[0])) // 2 - bb[0]
    y = (h - (bb[3] - bb[1])) // 2 - bb[1]
    d.text((x, y), ch, fill=255, font=f)
    px = img.load()
    pages = (h + 7) // 8
    out = []
    for p2 in range(pages):
        for cx in range(w):
            b = 0
            for r in range(8):
                yy = p2 * 8 + r
                if yy < h and px[cx, yy] > 110:
                    b |= (1 << r)
            out.append(b)
    return out

def main():
    ascii_font = pick(ASCII_FONTS)
    cjk_font = pick(CJK_FONTS)

    a0, a1 = ASCII_RANGE
    n_ascii = a1 - a0 + 1
    lines = [
        "/* 自动生成，请勿手改 —— 由 tools/生成字库.py 产出",
        " * 字表：ASCII 0x%02X~0x%02X（6×8，列优先）；中文 %d 字（16×16，列优先，每字 32 字节）" % (a0, a1, len(CN_CHARS)),
        " * 要新增汉字：改 tools/生成字库.py 的 CN_CHARS 后重跑脚本 + 重新编译 */",
        "#ifndef FONT_SUBSET_H",
        "#define FONT_SUBSET_H",
        "#include <stdint.h>",
        "",
        f"#define FONT_ASCII_FIRST 0x{a0:02X}",
        f"#define FONT_ASCII_COUNT {n_ascii}",
        f"#define FONT_CN_COUNT    {len(CN_CHARS)}",
        "",
        f"static const uint8_t FONT_ASCII[{n_ascii}][6] = {{",
    ]
    for i in range(n_ascii):
        b = bitmap(chr(a0 + i), 6, 8, ascii_font, 8)
        lines.append("    { " + ",".join(f"0x{v:02X}" for v in b) + " },   /* " +
                     (chr(a0 + i) if chr(a0 + i) != "\\" else "backslash") + " */")
    lines += ["};", ""]
    # 12×16 大号 ASCII：列优先，每列 2 字节（低字节=上 8 行，高字节=下 8 行）
    # 车载仪表盘用 —— 必须是"真字模"，放大 6×8 会吞笔画 ✗
    lines += [f"#define FONT_BIG_COUNT {len(BIG_CHARS)}",
              f"static const uint8_t FONT_BIG_UTF8[{len(BIG_CHARS)}] = {{ {",".join(str(ord(c)) for c in BIG_CHARS)} }};"]
    lines += [f"static const uint8_t FONT_ASCII_BIG[{len(BIG_CHARS)}][32] = {{"]
    for c in BIG_CHARS:
        b = bitmap(c, 16, 16, ascii_font, 16)
        lines.append("    {" + ",".join(f"0x{x:02X}" for x in b) + "},")
    lines += ["};", ""]
    lines += [f"static const uint8_t FONT_CN[{len(CN_CHARS)}][32] = {{"]
    for ch in CN_CHARS:
        b = bitmap(ch, 16, 16, cjk_font, 16)
        lines.append("    { " + ",".join(f"0x{v:02X}" for v in b) + f" }},   /* {ch} */")
    lines += ["};", ""]
    keys = ",".join("0x%02X" % x for ch in CN_CHARS for x in ch.encode("utf-8"))
    lines += ["/* 每个汉字的 UTF-8 三字节（用于查表） */",
              f"static const uint8_t FONT_CN_UTF8[{len(CN_CHARS) * 3}] = {{ {keys} }};", "",
              "#endif /* FONT_SUBSET_H */", ""]
    OUT.write_text("\n".join(lines), encoding="utf-8", newline="")
    print(f"[OK] 生成 {OUT}")
    print(f"     ASCII {n_ascii} 字（{n_ascii * 6} 字节） + 汉字 {len(CN_CHARS)} 字（{len(CN_CHARS) * 32} 字节）")
    print(f"     合计约 {(n_ascii * 6 + len(CN_CHARS) * 32 + len(CN_CHARS) * 3) / 1024:.1f} KB")
    print(f"     字体：ASCII={Path(ascii_font).name}  中文={Path(cjk_font).name}")


if __name__ == "__main__":
    main()
