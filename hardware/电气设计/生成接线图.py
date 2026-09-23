# -*- coding: utf-8 -*-
"""生成《全车电气连接》图纸 + 接线表 + netlist.json

**唯一事实来源**：本文件顶部的 PINMAP / FIGS。
改完重跑：  python "生成接线图.py" --build
输出（同目录）：
    02 驱动接线 / 03 感知接线 / 04 调试与扩展  (.tex/.pdf/.png)
    接线表.md      （人/机均可读的逐针定义）
    netlist.json   （机器可读网络表，供硬件 agent 直接消费）
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
OUT = HERE / "图纸"          # 图纸源码与产物都放这里
OUT.mkdir(exist_ok=True)
MIK = Path(r"C:\Users\35276\AppData\Local\Programs\MiKTeX\miktex\bin\x64")

VERSION = "v3.0"
DATE = "2026-09-20"

# ============================================================================
#  引脚分配（唯一事实来源）—— 依据 docs/引脚分配表.md v3.0
#  不可用：PC14 / PC15（板载 32.768kHz 晶振）、PD0 / PD1（板载 8MHz 晶振）
#  保留不用：PB2（BOOT1 —— 复位时被拉高会从 SRAM 启动，禁止接任何会拉高的信号）
#  需关 JTAG：PA15 / PB3 / PB4（本设计关了 JTAG 换取 3 路预留）
# ============================================================================
PINMAP = {
    "PA0": "VBAT_SENSE · ADC1_IN0（100k/10k 分压 + 100nF）",
    "PA1": "US_R_TRIG · 右超声波触发",
    "PA2": "USART2_TX · BLE 模块 RXD",
    "PA3": "USART2_RX · BLE 模块 TXD",
    "PA4": "US_L_TRIG · 左超声波触发",
    "PA5": "US_L_ECHO · 左超声波回波（分压）",
    "PA6": "ENC_R_C1 · TIM3_CH1 编码器",
    "PA7": "ENC_R_C2 · TIM3_CH2 编码器",
    "PA8": "PWM_L · TIM1_CH1（20kHz）",
    "PA9": "USART1_TX · 遥测（USB-TTL）",
    "PA10": "USART1_RX · 遥测（USB-TTL）",
    "PA11": "PWM_R · TIM1_CH4（20kHz，原 USB DM，只作输出）",
    "PA12": "KEY · 用户按键（按下接 GND）",
    "PA13": "SWDIO · 调试（锁死）",
    "PA14": "SWCLK · 调试（锁死）",
    "PA15": "预留 · 需关 JTAG",
    "PB0": "AIN1 · TB6612 左方向",
    "PB1": "AIN2 · TB6612 左方向",
    "PB2": "保留勿接 · BOOT1（拉高会从 SRAM 启动）",
    "PB3": "预留 · 需关 JTAG",
    "PB4": "预留 · 需关 JTAG",
    "PB5": "US_R_ECHO · 右超声波回波（分压；非 JTAG 脚，核心功能不依赖关 JTAG）",
    "PB6": "ENC_L_C1 · TIM4_CH1 编码器",
    "PB7": "ENC_L_C2 · TIM4_CH2 编码器",
    "PB8": "I2C1_SCL（重映射）· 灰度总线，上拉 5V",
    "PB9": "I2C1_SDA（重映射）· 灰度总线，上拉 5V",
    "PB10": "I2C2_SCL · OLED + MPU6050，上拉 3.3V",
    "PB11": "I2C2_SDA · OLED + MPU6050，上拉 3.3V",
    "PB12": "BIN1 · TB6612 右方向",
    "PB13": "BIN2 · TB6612 右方向",
    "PB14": "STBY · TB6612 使能（10k 上拉 + 跳线）",
    "PB15": "BUZZER · 蜂鸣器（1k → NPN）",
    "PC13": "状态 LED（核心板板载）",
}

TIMERS = {"TIM1": "PWM ×2（PA8/PA11）", "TIM2": "空闲", "TIM3": "右编码器（PA6/PA7）",
          "TIM4": "左编码器（PB6/PB7）", "SysTick": "1ms 时基", "DWT->CYCCNT": "超声波 µs 计时"}


def S(src, note, net, pin, name):
    """一行连接：源 / 源说明 / 网络名 / 连接器针号 / 连接器信号名"""
    return (src, note, net, pin, name)


# ============================================================================
#  图纸数据
# ============================================================================
FIGS = [
    dict(
        stem="02 驱动接线",
        title="驱动接线：TB6612FNG + 双编码器闭环 + 电机 6P 座",
        fill="电机侧 6P 线序与针距已由王政淋实测确认（M1/GND/C2/C1/VCC/M2）；编码器 VCC 实测走 +5V。",
        blocks=[
            dict(title="TB6612FNG 电机驱动模块", sub="方向线用 PB0/PB1/PB12/PB13（无需关 JTAG）；VM 大电流不走排针；AIN/BIN/STBY 各 10k 下拉",
                 rows=[
                     S("PA8", "TIM1_CH1", "PWM_L", "1", "PWMA"),
                     S("PA11", "TIM1_CH4", "PWM_R", "2", "PWMB"),
                     S("PB0", "", "AIN1_L", "3", "AIN1"),
                     S("PB1", "", "AIN2_L", "4", "AIN2"),
                     S("PB12", "", "BIN1_R", "5", "BIN1"),
                     S("PB13", "", "BIN2_R", "6", "BIN2"),
                     S("PB14", "10k 上拉 + 跳线", "STBY", "7", "STBY"),
                     S("3V3", "核心板 LDO", "3V3_LOGIC", "8", "VCC"),
                     S("GND", "", "GND", "9", "GND"),
                     S("12V", "470~1000µF 就近", "12V_BUS", "10", "VM"),
                 ]),
            dict(title="左电机 JST-XH 6P 座（电机自带线）", sub="针序=电机尾板实测：1 M1 / 2 GND / 3 C2 / 4 C1 / 5 VCC / 6 M2；C1/C2 串 100Ω、可选并 1nF",
                 rows=[
                     S("TB6612 AO1", "大电流", "MOTOR_L_M1", "1", "M1"),
                     S("GND", "", "GND", "2", "GND"),
                     S("PB7", "TIM4_CH2", "ENC_L_C2", "3", "C2"),
                     S("PB6", "TIM4_CH1", "ENC_L_C1", "4", "C1"),
                     S("5V", "实测编码器走 5V", "5V_BUS", "5", "VCC"),
                     S("TB6612 AO2", "大电流", "MOTOR_L_M2", "6", "M2"),
                 ]),
            dict(title="右电机 JST-XH 6P 座（电机自带线）", sub="", rows=[
                S("TB6612 BO1", "大电流", "MOTOR_R_M1", "1", "M1"),
                S("GND", "", "GND", "2", "GND"),
                S("PA7", "TIM3_CH2", "ENC_R_C2", "3", "C2"),
                S("PA6", "TIM3_CH1", "ENC_R_C1", "4", "C1"),
                S("5V", "实测编码器走 5V", "5V_BUS", "5", "VCC"),
                S("TB6612 BO2", "大电流", "MOTOR_R_M2", "6", "M2"),
            ]),
        ],
    ),
    dict(
        stem="03 感知接线",
        title="感知接线：灰度（感为主/亚博备）+ 双超声波 + 两条 I2C",
        fill="I2C1（灰度）上拉 5V；I2C2（OLED/MPU6050）上拉 3.3V —— 两条总线物理分开、绝不并联。",
        blocks=[
            dict(title="感为八路灰度（16P 排母，★主方案）", sub="I2C 模式可读数字量+模拟量；5V 供电 33mA；板载 5V/10k 上拉跳帽【不插】，由 PCB 侧 10k 上拉到 5V",
                 rows=[
                     S("GND", "①电源负", "GND", "1", "GND"),
                     S("5V", "②电源正 33mA", "5V_BUS", "2", "+5V"),
                     S("—", "按键：悬空", "GRAY_KEY", "3", "KEY"),
                     S("—", "错误输出：悬空（可选接排针观察）", "NC", "4", "Err"),
                     S("—", "并行数据：引排针、不接 MCU", "NC", "5-12", "1 ~ 8"),
                     S("—", "串行备份：引排针、不接 MCU", "GRAY_CLK", "13", "CLK"),
                     S("—", "串行备份：引排针、不接 MCU", "GRAY_DAT", "14", "DAT"),
                     S("PB8", "I2C1_SCL 重映射", "I2C_GRAY_SCL", "15", "SCL"),
                     S("PB9", "I2C1_SDA 重映射", "I2C_GRAY_SDA", "16", "SDA"),
                 ]),
            dict(title="亚博八路灰度 YB-MUX04（16P 排母，备用）", sub="针 1/2 与感为相反（5V 在 1 脚）→ 必须独立座；同样支持 I2C，共用 I2C1 的 SCL/SDA",
                 rows=[
                     S("5V", "①电源", "5V_BUS", "1", "5V"),
                     S("GND", "②接地", "GND", "2", "GND"),
                     S("—", "按键：悬空", "GRAY_KEY", "3", "KEY"),
                     S("—", "复位：悬空", "NC", "4", "RESET"),
                     S("—", "并行 IO：引排针、不接 MCU", "NC", "5-12", "X1 ~ X8"),
                     S("—", "串口：引排针、不接 MCU", "GRAY_TX", "13", "TX"),
                     S("—", "串口：引排针、不接 MCU", "GRAY_RX", "14", "RX"),
                     S("PB8", "I2C1_SCL 重映射", "I2C_GRAY_SCL", "15", "SCL"),
                     S("PB9", "I2C1_SDA 重映射", "I2C_GRAY_SDA", "16", "SDA"),
                 ]),
            dict(title="左超声波 HC-SR04（XH2.54-4P）", sub="两个 Trig 必须独立、软件分时触发；Echo 5V→3.11V 分压",
                 rows=[
                     S("5V", "", "5V_BUS", "1", "VCC"),
                     S("PA4", "", "US_L_TRIG", "2", "Trig"),
                     S("PA5", "2k/3.3k 分压", "US_L_ECHO", "3", "Echo"),
                     S("GND", "", "GND", "4", "GND"),
                 ]),
            dict(title="右超声波 HC-SR04（XH2.54-4P）", sub="Echo 用 PB5（非 JTAG 脚）—— 核心功能不依赖关 JTAG",
                 rows=[
                     S("5V", "", "5V_BUS", "1", "VCC"),
                     S("PA1", "", "US_R_TRIG", "2", "Trig"),
                     S("PB5", "2k/3.3k 分压", "US_R_ECHO", "3", "Echo"),
                     S("GND", "", "GND", "4", "GND"),
                 ]),
            dict(title="OLED 显示屏（XH2.54-4P）", sub="走 I2C2（3.3V 上拉）；与感为的 5V 总线完全隔离",
                 rows=[
                     S("3V3", "核心板 LDO", "3V3_LOGIC", "1", "VCC"),
                     S("GND", "", "GND", "2", "GND"),
                     S("PB10", "I2C2_SCL", "I2C_AUX_SCL", "3", "SCL"),
                     S("PB11", "I2C2_SDA", "I2C_AUX_SDA", "4", "SDA"),
                 ]),
            dict(title="MPU6050 六轴（XH2.54-4P）", sub="备用姿态；AD0 接地 → 7bit 地址 0x68（与 OLED 同总线，地址不冲突）",
                 rows=[
                     S("3V3", "核心板 LDO", "3V3_LOGIC", "1", "VCC"),
                     S("GND", "", "GND", "2", "GND"),
                     S("PB10", "I2C2_SCL", "I2C_AUX_SCL", "3", "SCL"),
                     S("PB11", "I2C2_SDA", "I2C_AUX_SDA", "4", "SDA"),
                 ]),
            dict(title="电池电压采样（分压）", sub="100k / 10k + 100nF；满电 12.6V → 1.15V，只做低压告警",
                 rows=[
                     S("12V", "经 100k", "VBAT_SENSE", "—", "分压中点 → PA0 (ADC1_IN0)"),
                 ]),
        ],
    ),
    dict(
        stem="04 调试与扩展",
        title="调试与扩展：BLE + 遥测串口 + SWD + 按键/蜂鸣器 + 预留",
        fill="遥测留在 USART1（与已冻结的遥测协议、已跑通的 MVP1 一致）；烧录、复位、救板都不需要拆车。",
        blocks=[
            dict(title="BLE 模块（实物 6P：GPIO1/VCC/GND/TXD/RXD/GPIO2）", sub="按实物做封装（不要按 HC-06 4P 底板猜）；VCC 取 3.3V（模块标称 2.2~5V）；天线端朝板边并留 keep-out",
                 rows=[
                     S("—", "GPIO1：悬空/预留", "NC", "1", "GPIO1"),
                     S("3V3", "核心板 LDO", "3V3_LOGIC", "2", "VCC"),
                     S("GND", "", "GND", "3", "GND"),
                     S("PA3", "USART2_RX", "BLE_TXD", "4", "TXD"),
                     S("PA2", "USART2_TX", "BLE_RXD", "5", "RXD"),
                     S("—", "GPIO2：悬空/预留", "NC", "6", "GPIO2"),
                 ]),
            dict(title="遥测串口 USB-TTL（XH2.54-4P）", sub="USART1 · 115200 8N1（后续可升 460800）· 帧格式见 docs/遥测协议.md（已冻结）",
                 rows=[
                     S("5V", "TTL 电源脚可不接", "5V_BUS", "1", "5V"),
                     S("PA9", "USART1_TX", "UART1_TX", "2", "RXD"),
                     S("PA10", "USART1_RX", "UART1_RX", "3", "TXD"),
                     S("GND", "", "GND", "4", "GND"),
                 ]),
            dict(title="SWD 调试排针（1×5）", sub="NRST 必须引出：关掉 JTAG 后靠它「连接下复位」救板（核心板自带 4pin SWD 没有 NRST）",
                 rows=[
                     S("3V3", "核心板 LDO", "3V3_LOGIC", "1", "3V3"),
                     S("PA13", "", "SWDIO", "2", "SWDIO"),
                     S("PA14", "", "SWCLK", "3", "SWCLK"),
                     S("NRST", "核心板 R 脚", "NRST", "4", "NRST"),
                     S("GND", "", "GND", "5", "GND"),
                 ]),
            dict(title="按键 / 蜂鸣器 / 指示灯", sub="核心板已自带复位键、BOOT0 跳线帽、电源灯 → PCB 上不重复",
                 rows=[
                     S("PA12", "板上已有 1.5k 上拉", "KEY", "—", "用户按键 → GND"),
                     S("PB15", "1k 基极电阻", "BUZZER", "—", "NPN(S8050) → 有源蜂鸣器 → 5V，并联续流二极管"),
                     S("12V", "1k 限流", "LED_12V", "—", "12V 电源指示灯 → GND"),
                 ]),
            dict(title="预留排针（2×4）", sub="3 路空闲 IO（PA15/PB3/PB4 需关 JTAG）+ 电源；示波器 / 逻辑分析仪 / 后续扩展",
                 rows=[
                     S("PA15", "需关 JTAG", "SPARE1", "1", "SPARE1"),
                     S("PB3", "需关 JTAG", "SPARE2", "2", "SPARE2"),
                     S("PB4", "需关 JTAG", "SPARE3", "3", "SPARE3"),
                     S("3V3", "核心板 LDO", "3V3_LOGIC", "4", "3V3"),
                     S("5V", "", "5V_BUS", "5", "5V"),
                     S("GND", "", "GND", "6", "GND"),
                     S("GND", "", "GND", "7", "GND"),
                     S("12V", "经保险丝+反接保护", "12V_BUS", "8", "12V"),
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
        "电源（12V_BUS / 5V_BUS / 3V3_LOGIC / GND）同样用网络标号表示，不画共用母线。"
        "逐针定义与电气约束见 电气连接规范.md；引脚契约见 docs/引脚分配表.md。")


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
    out = [f"# 接线表（网络表）{VERSION}", "",
           f"> 由 `生成接线图.py` 从同一份数据生成，**不要手改**——改数据后重跑脚本。",
           "> 引脚契约：`docs/引脚分配表.md`；电气约束与改动规则：`电气连接规范.md`。",
           "> 灰度主方案 = 感为（I2C1，5V 上拉）；备用 = 亚博（同一 I2C1 总线，独立 16P 座）。", ""]
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
                if src != "—":
                    e["fanout"].append({"role": "src", "endpoint": src, "note": note})
                e["fanout"].append({"role": "dst", "block": blk["title"], "pin": pin, "signal": name})
    doc = {
        "_readme": "全车电气连接网络表（机器可读）。生成自 生成接线图.py，勿手改；改数据后重跑脚本。",
        "_version": VERSION,
        "_authority": ["docs/引脚分配表.md", "hardware/电气设计/电气连接规范.md"],
        "board": {
            "mcu": "STM32F103C8T6 最小系统板（Blue Pill 克隆；micro-USB；板载 8MHz + 32.768kHz 晶振；板载 RT9193 LDO）",
            "pcb": "单板（功率与逻辑同板）",
            "unusable_pins": {
                "PD0": "8MHz 晶振占用", "PD1": "8MHz 晶振占用",
                "PC14": "32.768kHz 晶振占用", "PC15": "32.768kHz 晶振占用",
                "PB2": "BOOT1 —— 复位时被拉高会从 SRAM 启动，禁止接任何会拉高的信号/按键",
                "PA13/PA14": "保留给 SWD，不得作 GPIO",
            },
            "jtag": "关闭 JTAG 保留 SWD（__HAL_AFIO_REMAP_SWJ_NOJTAG）→ 释放 PA15/PB3/PB4 作预留；NRST 已引到 SWD 排针，可用「连接下复位」恢复",
            "supply": ("单电源 12V 锂电 → DC005/2P焊盘 → T3A 慢断保险丝 → AO4407A P-MOS 反接保护 → 总开关 → 12V_BUS；"
                       "MP1584 → 5V_BUS；3V3_LOGIC 由核心板 RT9193 输出（≤300mA）"),
            "power_buses": ["12V_BUS", "5V_BUS", "3V3_LOGIC", "GND"],
            "power_rules": [
                "核心板 5V 脚与 3V3 脚只能接一个（本项目接 5V）；本板不得自行产生 3.3V",
                "I2C1（灰度）上拉 5V；I2C2（OLED+MPU6050）上拉 3.3V；两条总线绝不并联",
            ],
        },
        "pinmap": PINMAP,
        "timers": TIMERS,
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
        shutil.copy2(OUT / (stem + ".tex"), tmp / "fig.tex")
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
        shutil.copy2(tmp / "fig.pdf", OUT / (stem + ".pdf"))
        shutil.copy2(png, OUT / (stem + ".png"))
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
        (OUT / (fig["stem"] + ".tex")).write_text(emit_tex(fig), encoding="utf-8")
        stems.append(fig["stem"])
    (HERE / "接线表.md").write_text(emit_md(), encoding="utf-8")
    (HERE / "netlist.json").write_text(emit_json(), encoding="utf-8")
    print("[OK] 生成 .tex x%d → 图纸/ + 接线表.md + netlist.json (%s)" % (len(stems), VERSION))
    if args.build:
        if (OUT / "01 电源与保护.tex").exists():
            stems.append("01 电源与保护")
        return 0 if build(stems) else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
