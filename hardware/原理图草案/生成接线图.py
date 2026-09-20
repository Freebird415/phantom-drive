# -*- coding: utf-8 -*-
"""生成《全车电气连接》图纸 + 接线表 + netlist.json

**唯一事实来源**：本文件顶部的 NETS / FIGS 数据。
改完重跑：  python 生成接线图.py --build
输出（同目录）：
    02 驱动接线.tex/.pdf/.png      03 感知接线.tex/.pdf/.png      04 调试与扩展.tex/.pdf/.png
    接线表.md      （人/机均可读的逐针定义）
    netlist.json   （机器可读网络表，供王政淋的 agent 直接消费）

01 电源与保护.tex 是手写元件级电路，不由本脚本生成（但由本脚本编译）。
编译在 ASCII 临时目录里做（Windows 中文路径下 xelatex / pdftoppm 不稳），再拷回仓库。
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
MIK = Path(r"C:\Users\35276\AppData\Local\Programs\MiKTeX\miktex\bin\x64")

# ============================================================================
#  引脚分配（唯一事实来源）—— 依据 docs/引脚分配表.md v0.2
#  说明：PC14/PC15 被板上 32.768kHz 晶振占用、PD0/PD1 被 8MHz 晶振占用 → 均不可用
# ============================================================================
PINMAP = {
    "PA0": "ENC_L_A · TIM2_CH1 编码器",
    "PA1": "ENC_L_B · TIM2_CH2 编码器",
    "PA2": "USART2_TX · HC-06",
    "PA3": "USART2_RX · HC-06",
    "PA4": "US_L_TRIG",
    "PA5": "US_L_ECHO（2k/3.3k 分压）",
    "PA6": "ENC_R_A · TIM3_CH1 编码器",
    "PA7": "ENC_R_B · TIM3_CH2 编码器",
    "PA8": "US_R_TRIG",
    "PA9": "USART1_TX · 遥测",
    "PA10": "USART1_RX · 遥测",
    "PA11": "STBY · TB6612 使能（原 USB DM，只作输出）",
    "PA12": "预留（原 USB DP，板上 1.5k 上拉，勿作输入）",
    "PA13": "SWDIO",
    "PA14": "SWCLK",
    "PA15": "US_R_ECHO（需关 JTAG）",
    "PB0": "VBAT_SENSE · ADC1_IN8（100k/10k 分压）",
    "PB1": "BUZZER（蜂鸣器，定时器中断翻转）",
    "PB2": "KEY（用户按键，10k 上拉 + 100nF 去抖）",
    "PB3": "预留（需关 JTAG）",
    "PB4": "预留（需关 JTAG）",
    "PB5": "预留",
    "PB6": "PWM_L · TIM4_CH1（20 kHz）",
    "PB7": "PWM_R · TIM4_CH2（20 kHz）",
    "PB8": "I2C1_SCL（重映射）· 灰度 5V 总线",
    "PB9": "I2C1_SDA（重映射）· 灰度 5V 总线",
    "PB10": "I2C2_SCL · OLED + MPU6050（3.3V 总线）",
    "PB11": "I2C2_SDA · OLED + MPU6050（3.3V 总线）",
    "PB12": "AIN1 · TB6612 左方向",
    "PB13": "AIN2 · TB6612 左方向",
    "PB14": "BIN1 · TB6612 右方向",
    "PB15": "BIN2 · TB6612 右方向",
    "PC13": "状态 LED（核心板板载）",
}

# ============================================================================
#  图纸数据：每行 = (源, 源说明, 网络名, 连接器针号, 连接器信号名)
# ============================================================================
def S(src, note, net, pin, name):
    return (src, note, net, pin, name)


FIGS = [
    dict(
        stem="02 驱动接线",
        title="驱动接线：TB6612FNG + 双编码器闭环",
        fill="TB6612FNG 控制线、电机大电流回路、两路编码器（闭环测速）。电机侧用现成 JST-XH 6P 座。",
        blocks=[
            dict(title="TB6612FNG 电机驱动模块", sub="方向线全部走 PB12~PB15（不需要关 JTAG）；VM 走大电流焊盘", rows=[
                S("PB6", "TIM4_CH1", "PWM_L", "1", "PWMA"),
                S("PB7", "TIM4_CH2", "PWM_R", "2", "PWMB"),
                S("PB12", "", "AIN1_L", "3", "AIN1"),
                S("PB13", "", "AIN2_L", "4", "AIN2"),
                S("PB14", "", "BIN1_R", "5", "BIN1"),
                S("PB15", "", "BIN2_R", "6", "BIN2"),
                S("PA11", "硬件 10k 上拉 + 跳线可选常使能", "STBY", "7", "STBY"),
                S("5V", "模块自带 3.3V LDO 供 3V3", "5V", "8", "VCC"),
                S("GND", "", "GND", "9", "GND"),
                S("12V", "大电流焊盘 + 470µF", "VM", "10", "VM"),
            ]),
            dict(title="左电机 JST-XH 6P 座（现成插头）", sub="同一个座里同时走电机电流与编码器信号 —— 线序必须实测复核；A/B 接反只需软件换向",
                 rows=[
                     S("左电机", "大电流", "MOT_L+", "1", "M+"),
                     S("左电机", "大电流", "MOT_L-", "2", "M−"),
                     S("3V3", "跳线可选 5V", "ENC_VCC", "3", "编码器 VCC"),
                     S("GND", "", "GND", "4", "GND"),
                     S("PA0", "TIM2_CH1", "ENC_L_A", "5", "A"),
                     S("PA1", "TIM2_CH2", "ENC_L_B", "6", "B"),
                 ]),
            dict(title="右电机 JST-XH 6P 座（现成插头）", sub="", rows=[
                S("右电机", "大电流", "MOT_R+", "1", "M+"),
                S("右电机", "大电流", "MOT_R-", "2", "M−"),
                S("3V3", "跳线可选 5V", "ENC_VCC", "3", "编码器 VCC"),
                S("GND", "", "GND", "4", "GND"),
                S("PA6", "TIM3_CH1", "ENC_R_A", "5", "A"),
                S("PA7", "TIM3_CH2", "ENC_R_B", "6", "B"),
            ]),
        ],
    ),
    dict(
        stem="03 感知接线",
        title="感知接线：灰度（感为主/亚博备）+ 超声波 + 两条 I2C",
        fill="感为走 5V 上拉的 I2C1；OLED / MPU6050 走 3.3V 上拉的 I2C2 —— 两条总线绝不并联。",
        blocks=[
            dict(title="感为八路灰度（16P，★主方案）", sub="I2C 模式：可读数字量+模拟量；5V 供电 33mA；上拉用感为板载 5V/10k 跳线（拔掉 PCB 侧上拉）",
                 rows=[
                     S("GND", "①电源负", "GND", "1", "GND"),
                     S("5V", "②电源正 33mA", "5V", "2", "+5V"),
                     S("KEY", "按键（悬空）", "GRAY_KEY", "3", "KEY"),
                     S("—", "错误输出（可选接 PA11 观察）", "GRAY_ERR", "4", "Err"),
                     S("—", "并行数据 1~8：本设计不用", "NC", "5-12", "1 ~ 8"),
                     S("—", "串行备份：留排针可跳线", "GRAY_CLK", "13", "CLK"),
                     S("—", "串行备份：留排针可跳线", "GRAY_DAT", "14", "DAT"),
                     S("PB8", "I2C1_SCL 重映射", "I2C_GRAY_SCL", "15", "SCL"),
                     S("PB9", "I2C1_SDA 重映射", "I2C_GRAY_SDA", "16", "SDA"),
                 ]),
            dict(title="亚博八路灰度（16P，备用）", sub="针 1/2 与感为相反（5V 在 1 脚）→ 必须独立座子，不可和感为共用；同样支持 I2C",
                 rows=[
                     S("5V", "①电源", "5V", "1", "5V"),
                     S("GND", "②接地", "GND", "2", "GND"),
                     S("KEY", "按键（悬空）", "GRAY_KEY", "3", "KEY"),
                     S("—", "复位（悬空）", "NC", "4", "RESET"),
                     S("—", "并行 IO X1~X8：本设计不用", "NC", "5-12", "X1 ~ X8"),
                     S("—", "串口 TX/RX：留排针可跳线", "GRAY_TX", "13", "TX"),
                     S("—", "串口 TX/RX：留排针可跳线", "GRAY_RX", "14", "RX"),
                     S("PB8", "I2C1_SCL 重映射", "I2C_GRAY_SCL", "15", "SCL"),
                     S("PB9", "I2C1_SDA 重映射", "I2C_GRAY_SDA", "16", "SDA"),
                 ]),
            dict(title="超声波 1（左，XH2.54-4P）", sub="两模块 Trig 独立、软件分时触发（防串扰）；Echo 5V→3.1V 分压", rows=[
                S("5V", "", "5V", "1", "VCC"),
                S("PA4", "", "US_L_TRIG", "2", "Trig"),
                S("PA5", "2k/3.3k 分压", "US_L_ECHO", "3", "Echo"),
                S("GND", "", "GND", "4", "GND"),
            ]),
            dict(title="超声波 2（右，XH2.54-4P）", sub="", rows=[
                S("5V", "", "5V", "1", "VCC"),
                S("PA8", "", "US_R_TRIG", "2", "Trig"),
                S("PA15", "关 JTAG", "US_R_ECHO", "3", "Echo"),
                S("GND", "", "GND", "4", "GND"),
            ]),
            dict(title="OLED 显示屏（XH2.54-4P）", sub="走 I2C2（3.3V 上拉）；与 MPU6050 同总线、与灰度总线完全隔离", rows=[
                S("3V3", "", "3V3", "1", "VCC"),
                S("GND", "", "GND", "2", "GND"),
                S("PB10", "I2C2_SCL", "I2C_AUX_SCL", "3", "SCL"),
                S("PB11", "I2C2_SDA", "I2C_AUX_SDA", "4", "SDA"),
            ]),
            dict(title="MPU6050 六轴（XH2.54-4P）", sub="备用，暂不参与控制；AD0 接地 → 地址 0x68", rows=[
                S("3V3", "", "3V3", "1", "VCC"),
                S("GND", "", "GND", "2", "GND"),
                S("PB10", "I2C2_SCL", "I2C_AUX_SCL", "3", "SCL"),
                S("PB11", "I2C2_SDA", "I2C_AUX_SDA", "4", "SDA"),
            ]),
            dict(title="12V 电压检测（分压）", sub="100k / 10k 分压 + 100nF；只做低压告警，不参与控制", rows=[
                S("12V", "经 100k", "VBAT_SENSE", "—", "分压中点 → PB0 (ADC1_IN8)"),
            ]),
        ],
    ),
    dict(
        stem="04 调试与扩展",
        title="调试与扩展：蓝牙 + 遥测串口 + SWD + 按键/蜂鸣器 + 预留",
        fill="烧录、复位、救砖、看串口都不需要拆车；空闲 IO 全部引出到预留排针。",
        blocks=[
            dict(title="HC-06 蓝牙（XH2.54-4P）", sub="仅调试用；比赛时不接（全自动、禁遥控）", rows=[
                S("5V", "裸模块为 3.3V", "5V", "1", "VCC"),
                S("GND", "", "GND", "2", "GND"),
                S("PA3", "USART2_RX", "BT_TX", "3", "TX"),
                S("PA2", "USART2_TX", "BT_RX", "4", "RX"),
            ]),
            dict(title="遥测串口 USB-TTL（XH2.54-4P）", sub="接笔记本，115200 8N1（后续可升 460800）", rows=[
                S("5V", "TTL 电源脚可不接", "5V", "1", "5V"),
                S("PA9", "USART1_TX", "UART1_TX", "2", "RXD"),
                S("PA10", "USART1_RX", "UART1_RX", "3", "TXD"),
                S("GND", "", "GND", "4", "GND"),
            ]),
            dict(title="SWD 调试排针（1×5）", sub="NRST 必须引出：关掉 JTAG 后靠它连接下复位救板", rows=[
                S("3V3", "来自核心板", "3V3", "1", "3V3"),
                S("PA13", "", "SWDIO", "2", "SWDIO"),
                S("PA14", "", "SWCLK", "3", "SWCLK"),
                S("NRST", "核心板 R 脚", "NRST", "4", "NRST"),
                S("GND", "", "GND", "5", "GND"),
            ]),
            dict(title="按键 / 蜂鸣器 / 指示灯", sub="核心板已自带复位键、BOOT0 跳线帽、电源灯 → PCB 上不再重复", rows=[
                S("PB2", "10k 上拉 + 100nF", "KEY", "—", "用户按键 → GND"),
                S("PB1", "定时器中断翻转方波", "BUZZER", "—", "有源蜂鸣器 + NPN 三极管 + 1k 基极电阻"),
                S("12V", "1k 限流", "LED_12V", "—", "12V 电源指示灯 → GND"),
            ]),
            dict(title="预留排针区（2×4）", sub="空闲 IO + 电源；示波器 / 逻辑分析仪 / 后续扩展。PA12 只作输出", rows=[
                S("PB3", "需关 JTAG", "SPARE1", "1", "SPARE1"),
                S("PB4", "需关 JTAG", "SPARE2", "2", "SPARE2"),
                S("PB5", "空闲", "SPARE3", "3", "SPARE3"),
                S("PA12", "原 USB DP，1.5k 上拉，勿作输入", "SPARE4", "4", "SPARE4"),
                S("3V3", "来自核心板", "3V3", "5", "3V3"),
                S("5V", "", "5V", "6", "5V"),
                S("GND", "", "GND", "7", "GND"),
                S("12V", "经 3A 保险丝", "12V", "8", "12V"),
            ]),
        ],
    ),
]

PREAMBLE = r"""\documentclass{article}
\usepackage[paperwidth=60cm,paperheight=120cm,margin=0pt]{geometry}
\usepackage{fontspec}\setmainfont{Latin Modern Roman}
\usepackage{xeCJK}\setCJKmainfont{Microsoft YaHei}
\usepackage{amsmath, amssymb, siunitx}
\usepackage[american]{circuitikz}
\ctikzset{bipoles/length=1.2cm}
% 不用 standalone：它自家的裁边在 xelatex 下不生效，长图会被 letter 页面截掉尾部。
% 改用 preview 把页面尺寸严格设成内容尺寸（自动裁边）。
\usepackage[active,tightpage]{preview}
\PreviewEnvironment{circuitikz}
\setlength\PreviewBorder{8pt}
\pagestyle{empty}
\begin{document}
"""

ROW_H = 0.74
GAP = 0.60
X_SRC, X_WIRE0, X_BOX0, X_BOX1 = 4.55, 4.85, 12.30, 18.40
X_NET = (X_WIRE0 + X_BOX0) / 2

FOOT = ("网络名 = 嘉立创 EDA 里的网络标号：两边同名即同一根线。"
        "电源（5V / 3V3 / GND / 12V）同样用网络标号表示，不画共用母线。"
        "逐针定义、电气约束与改动规则见 电气连接规范.md；引脚契约见 docs/引脚分配表.md。")


def esc(t: str) -> str:
    return t.replace("_", r"\_").replace("%", r"\%").replace("&", r"\&").replace("#", r"\#")


def emit_tex(fig: dict) -> str:
    L = [PREAMBLE, r"\begin{circuitikz}", r"\small"]
    y = 0.0
    L.append(r"\node[anchor=east, font=\bfseries\small] at (%.2f,%.2f) {MCU\ /\ 电源};" % (X_SRC, y))
    L.append(r"\node[anchor=west, font=\bfseries\small] at (%.2f,%.2f) {网络名};" % (X_WIRE0 + 0.1, y))
    L.append(r"\node[anchor=west, font=\bfseries\small] at (%.2f,%.2f) {连接器针脚};" % (X_BOX0 + 0.15, y))
    y -= GAP
    for blk in fig["blocks"]:
        rows = blk["rows"]
        n = len(rows)
        L.append(r"\node[anchor=west, font=\bfseries\small] at (%.2f,%.2f) {%s};" % (X_BOX0, y, blk["title"]))
        if blk.get("sub"):
            L.append(r"\node[anchor=west, font=\scriptsize\itshape] at (%.2f,%.2f) {%s};"
                     % (X_BOX0, y - 0.38, blk["sub"]))
            top = y - 0.82
        else:
            top = y - 0.42
        y0 = top - 0.42
        for i, (src, note, net, pin, name) in enumerate(rows):
            yy = y0 - i * ROW_H
            label = esc(src) + (r"\ \textperiodcentered\ " + esc(note) if note else "")
            L.append(r"\node[anchor=east, font=\small] at (%.2f,%.2f) {%s};" % (X_SRC, yy, label))
            L.append(r"\draw (%.2f,%.2f) -- (%.2f,%.2f);" % (X_WIRE0, yy, X_BOX0, yy))
            L.append(r"\node[anchor=south, font=\scriptsize\ttfamily] at (%.2f,%.2f) {%s};"
                     % (X_NET, yy + 0.05, esc(net)))
            L.append(r"\node[anchor=west, font=\small] at (%.2f,%.2f) {%s\,--\,%s};"
                     % (X_BOX0 + 0.18, yy, esc(pin), esc(name)))
        bot = y0 - (n - 1) * ROW_H - 0.30
        L.append(r"\draw[rounded corners=2pt] (%.2f,%.2f) rectangle (%.2f,%.2f);" % (X_BOX0, top, X_BOX1, bot))
        y = bot - GAP
    y -= 0.30
    L.append(r"\node[anchor=west, font=\scriptsize\itshape] at (0,%.2f) {%s};" % (y, esc(FOOT)))
    L += [r"\end{circuitikz}", r"\end{document}"]
    return "\n".join(L) + "\n"


def emit_md() -> str:
    out = ["# 接线表（网络表）", "",
           "> 由 `生成接线图.py` 从同一份数据生成，**不要手改**——改数据后重跑脚本。",
           "> 引脚契约：`docs/引脚分配表.md`；电气约束与改动规则：`电气连接规范.md`。",
           "> 灰度主方案 = 感为（I2C1, 5V 上拉）；备用 = 亚博（同一 I2C1 总线，独立座）。", ""]
    for fig in FIGS:
        out += [f"## {fig['title']}", "", f"> {fig['fill']}", ""]
        for blk in fig["blocks"]:
            out += [f"### {blk['title']}", ""]
            if blk.get("sub"):
                out += [f"> {blk['sub']}", ""]
            out += ["| 网络名 | MCU / 电源 | 备注 | 连接器针号 | 连接器信号 |", "|---|---|---|---|---|"]
            for src, note, net, pin, name in blk["rows"]:
                out.append(f"| `{net}` | {src} | {note or '—'} | {pin} | {name} |")
            out.append("")
    return "\n".join(out) + "\n"


def emit_json() -> str:
    nets = {}
    for fig in FIGS:
        for blk in fig["blocks"]:
            for src, note, net, pin, name in blk["rows"]:
                if net == "NC":
                    continue
                e = nets.setdefault(net, {"net": net, "fanout": []})
                e["fanout"].append({"role": "src", "endpoint": src, "note": note} if src not in ("—",)
                                   else {"role": "nc", "endpoint": name, "note": note})
                e["fanout"].append({"role": "dst", "block": blk["title"], "pin": pin, "signal": name})
    doc = {
        "_readme": "全车电气连接网络表（机器可读）。生成自 生成接线图.py，勿手改；改数据后重跑脚本。",
        "_authority": ["docs/引脚分配表.md", "hardware/原理图草案/电气连接规范.md"],
        "_generated": "2026-09-20",
        "board": {
            "mcu": "STM32F103C8T6 最小系统板（Blue Pill 克隆；micro-USB；板载 8MHz + 32.768kHz 晶振）",
            "unusable_pins": {"PD0": "8MHz 晶振占用", "PD1": "8MHz 晶振占用", "PC14": "32.768kHz 晶振占用", "PC15": "32.768kHz 晶振占用"},
            "supply": "单电源 12V 锂电 → 3A 慢断保险丝 → P-MOS 反接保护 → TB6612 VM；MP1584→5V；3.3V 由核心板 RT9193 输出",
            "power_buses": ["12V", "5V", "3V3", "GND"],
        },
        "pinmap": PINMAP,
        "nets": sorted(nets.values(), key=lambda x: x["net"]),
    }
    return json.dumps(doc, ensure_ascii=False, indent=2) + "\n"


def build(stems: list[str]) -> bool:
    ok = True
    tmp = Path(tempfile.mkdtemp(prefix="pd_schem_"))
    trim = Path(r"C:\Users\35276\.pi\agent\skills\circuitikz-circuit-renderer\scripts\trim-white.ps1")
    for stem in stems:
        for f in list(tmp.glob("fig.*")) + list(tmp.glob("*.png")):
            try:
                f.unlink()
            except FileNotFoundError:
                pass
        shutil.copy2(HERE / (stem + ".tex"), tmp / "fig.tex")
        r1 = subprocess.run([str(MIK / "xelatex.exe"), "-interaction=nonstopmode", "-halt-on-error", "fig.tex"],
                            cwd=tmp, capture_output=True)
        if r1.returncode != 0 or not (tmp / "fig.pdf").exists():
            print(f"[FAIL] xelatex 第一遍 {stem}")
            log = tmp / "fig.log"
            print(log.read_text(errors="replace")[-1800:] if log.exists() else r1.stdout.decode(errors="replace")[-1800:])
            ok = False
            continue
        subprocess.run([str(MIK / "xelatex.exe"), "-interaction=nonstopmode", "-halt-on-error", "fig.tex"],
                       cwd=tmp, capture_output=True)
        if not (tmp / "fig.pdf").exists():
            print(f"[FAIL] 第二遍无 PDF {stem}")
            ok = False
            continue
        subprocess.run([str(MIK / "pdftoppm.exe"), "-r", "200", "-png", "fig.pdf", "fig"],
                       cwd=tmp, capture_output=True)
        if not (tmp / "fig-1.png").exists():
            print(f"[FAIL] pdftoppm {stem}")
            ok = False
            continue
        t = subprocess.run(["powershell", "-ExecutionPolicy", "Bypass", "-File", str(trim),
                            "-InputPath", str(tmp / "fig-1.png"), "-OutputPath", str(tmp / "fig.png"),
                            "-Padding", "40"], capture_output=True)
        png = tmp / "fig.png" if (t.returncode == 0 and (tmp / "fig.png").exists()) else (tmp / "fig-1.png")
        shutil.copy2(tmp / "fig.pdf", HERE / (stem + ".pdf"))
        shutil.copy2(png, HERE / (stem + ".png"))
        size = ""
        info = subprocess.run([str(MIK / "pdfinfo.exe"), str(tmp / "fig.pdf")], capture_output=True)
        for ln in info.stdout.decode(errors="replace").splitlines():
            if ln.startswith("Page size"):
                size = ln.split(":", 1)[1].strip()
        print(f"[OK] {stem}.pdf / {stem}.png   ({size})")
    shutil.rmtree(tmp, ignore_errors=True)
    return ok


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", action="store_true", help="编译 pdf / png")
    args = ap.parse_args()
    stems = []
    for fig in FIGS:
        (HERE / (fig["stem"] + ".tex")).write_text(emit_tex(fig), encoding="utf-8")
        stems.append(fig["stem"])
    (HERE / "接线表.md").write_text(emit_md(), encoding="utf-8")
    (HERE / "netlist.json").write_text(emit_json(), encoding="utf-8")
    print("[OK] 生成 .tex x%d + 接线表.md + netlist.json" % len(stems))
    if args.build:
        if (HERE / "01 电源与保护.tex").exists():
            stems.append("01 电源与保护")
        return 0 if build(stems) else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
