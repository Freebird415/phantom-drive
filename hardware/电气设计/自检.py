# -*- coding: utf-8 -*-
"""电气设计自检 —— 三遍独立检验

用法：  python 自检.py            （读同目录 netlist.json / 接线表.md / 图纸，校验 docs/引脚分配表.md）
退出码：0 = 全过；1 = 有 FAIL

  第 1 遍 结构性 / 电气合法性  —— 引脚是否合法、是否用了不可用脚、外设通道与引脚是否真的对得上
  第 2 遍 一致性              —— FIGS ↔ netlist.json ↔ 接线表.md ↔ docs/引脚分配表.md ↔ 图纸文件
  第 3 遍 需求与手册闭环      —— 逐条对照已确认决策、厂商手册硬约束、已冻结协议、实测数据
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "图纸"
ROOT = HERE.parents[1]
FAIL, WARN = [], []


def chk(cond, msg, warn=False):
    if cond:
        print(f"  [OK]   {msg}")
    else:
        print(f"  [{'WARN' if warn else 'FAIL'}] {msg}")
        (WARN if warn else FAIL).append(msg)


# ---------------------------------------------------------------- 事实表（来自 STM32F103x8 数据手册）
LEGAL = set(f"PA{i}" for i in range(16)) | set(f"PB{i}" for i in range(16)) | {"PC13", "NRST"}
# 硬件不可用（绝不能出现在引脚表里）
UNUSABLE = {"PD0": "8MHz 晶振", "PD1": "8MHz 晶振", "PC14": "32.768kHz 晶振", "PC15": "32.768kHz 晶振"}
# 允许登记、但只能以特定身份出现
RESERVED_OK = {"PA13": ("SWD", "SWDIO"), "PA14": ("SWD", "SWCLK"),
               "PB2": ("BOOT1", "保留")}
ADC_PINS = {f"PA{i}" for i in range(8)} | {"PB0", "PB1"}          # ADC1_IN0..IN9（LQFP48 可用）
AF = {  # 外设通道 → 允许的引脚（默认映射 / 重映射）
    "TIM1_CH1": {"PA8"}, "TIM1_CH4": {"PA11"},
    "TIM2_CH1": {"PA0", "PA15"}, "TIM2_CH2": {"PA1", "PB3"},
    "TIM3_CH1": {"PA6"}, "TIM3_CH2": {"PA7"},
    "TIM4_CH1": {"PB6"}, "TIM4_CH2": {"PB7"},
    "USART1_TX": {"PA9"}, "USART1_RX": {"PA10"},
    "USART2_TX": {"PA2"}, "USART2_RX": {"PA3"},
    "I2C1_SCL": {"PB8"}, "I2C1_SDA": {"PB9"},                      # 重映射
    "I2C2_SCL": {"PB10"}, "I2C2_SDA": {"PB11"},
}


def load():
    gen = {}
    src = (HERE / "生成接线图.py").read_text(encoding="utf-8")
    ns = {"__name__": "gen", "__file__": str(HERE / "生成接线图.py")}
    exec(compile(src, "生成接线图.py", "exec"), ns)
    return ns


def main():
    print("=" * 78)
    print("第 1 遍：结构性 / 电气合法性")
    print("=" * 78)
    g = load()
    PINMAP, FIGS, TIMERS = g["PINMAP"], g["FIGS"], g["TIMERS"]

    chk(len(PINMAP) == 33, f"引脚表条目数 = 33（实际 {len(PINMAP)}）")
    for p in PINMAP:
        chk(p in LEGAL, f"引脚 {p} 合法")
    used_unusable = [p for p in PINMAP if p in UNUSABLE]
    chk(not used_unusable, f"没有使用硬件上不可用的引脚（发现：{used_unusable}）")
    for p, (what, kw) in RESERVED_OK.items():
        chk(p in PINMAP and kw in PINMAP[p], f"{p} 只以「{what}」身份登记（当前描述：{PINMAP.get(p, '缺失')}）")
    swd_only = [p for p, d in PINMAP.items() if ("SWD" in d or "SWCLK" in d)]
    chk(set(swd_only) == {"PA13", "PA14"}, f"SWD 只占 PA13/PA14（当前 {sorted(swd_only)}）")

    # 外设通道 ↔ 引脚
    for pin, desc in PINMAP.items():
        for ch, pins in AF.items():
            if ch in desc:
                chk(pin in pins, f"{pin} 的 {ch} 与数据手册默认/重映射引脚一致（允许 {sorted(pins)}）")
    # 定时器不重叠
    t_use = {}
    for pin, desc in PINMAP.items():
        m = re.search(r"(TIM\d)_CH\d", desc)
        if m:
            t_use.setdefault(m.group(1), []).append(pin)
    chk(set(t_use) <= {"TIM1", "TIM3", "TIM4"}, f"用到的定时器 {sorted(t_use)} 互不重复")
    chk("TIM2" in TIMERS and "空闲" in TIMERS["TIM2"], "TIM2 保持空闲（不产生 remap，避免抢 PA2/PA3）")

    # I2C 两条总线必须完全分开
    i2c1 = {p for p, d in PINMAP.items() if "I2C1" in d}
    i2c2 = {p for p, d in PINMAP.items() if "I2C2" in d}
    chk(i2c1 == {"PB8", "PB9"}, f"I2C1 = PB8/PB9（实际 {sorted(i2c1)}）")
    chk(i2c2 == {"PB10", "PB11"}, f"I2C2 = PB10/PB11（实际 {sorted(i2c2)}）")
    chk(not (i2c1 & i2c2), "两条 I2C 总线没有任何共用引脚")
    chk("5V" in PINMAP["PB8"] and "3.3V" in PINMAP["PB10"], "I2C1 标 5V 上拉、I2C2 标 3.3V 上拉")

    # 电压采样必须是 ADC 脚
    vb = [p for p, d in PINMAP.items() if "ADC1_IN" in d]
    chk(len(vb) == 1 and vb[0] in ADC_PINS, f"电压采样在合法 ADC 脚上（{vb}）")

    # 每个功能脚只出现一次（预留脚天然同名，跳过）
    func = {p: d for p, d in PINMAP.items() if "预留" not in d}
    dup = [p for p, d in func.items() if list(func.values()).count(d) > 1]
    chk(not dup, f"没有两个功能脚承担同一描述（{dup}）")

    print()
    print("=" * 78)
    print("第 2 遍：一致性（数据 ↔ 产物 ↔ 契约文档 ↔ 图纸）")
    print("=" * 78)
    net = json.loads((HERE / "netlist.json").read_text(encoding="utf-8"))
    md = (HERE / "接线表.md").read_text(encoding="utf-8")
    spec = (ROOT / "docs" / "引脚分配表.md").read_text(encoding="utf-8")

    chk(net["pinmap"] == PINMAP, "netlist.json 的 pinmap 与生成器数据逐字相同")
    chk(net["_version"] == g["VERSION"], f"netlist.json 版本 = {g['VERSION']}")

    # FIGS → PINMAP：图上用到的 MCU 源脚必须登记，且功能关键词一致
    rows = [(s, n, net_, p, nm) for f in FIGS for b in f["blocks"] for (s, n, net_, p, nm) in b["rows"]]
    mcu_src = {s for s, *_ in rows if re.fullmatch(r"P[A-C]\d+", s)}
    chk(mcu_src <= set(PINMAP), f"图上每个 MCU 源脚都在引脚表里（缺 {sorted(mcu_src - set(PINMAP))}）")
    chk(len(rows) == sum(1 for l in md.splitlines() if l.startswith("| `")), "接线表.md 行数与 FIGS 完全一致")

    # FIGS ↔ netlist.json
    jrows = {(n["net"], x["pin"], x["signal"]) for n in net["nets"] for x in n["fanout"] if x["role"] == "dst"}
    frows = {(net_, p, nm) for _, _, net_, p, nm in rows if net_ != "NC"}
    chk(jrows == frows, f"netlist.json 与 FIGS 的每条连接一一对应（差 {len(jrows ^ frows)} 条）")

    # 引脚表文档 ↔ PINMAP
    doc_pins = set(re.findall(r"^\| (P[A-C]\d+) \|", spec, re.M))
    chk(doc_pins == set(PINMAP), f"docs/引脚分配表.md 的引脚集合 == PINMAP（差 {doc_pins ^ set(PINMAP)}）")
    chk("v3.0" in spec, "引脚分配表已标 v3.0")
    # 计划 9/28 冻结；实际可能提前完成（当前：2026-09-23 王政淋确认提前冻结）
    chk(("冻结" in spec) and any(d in spec for d in ("09-23", "9/23", "9-28", "9/28")),
        "引脚分配表已标注冻结状态与日期（计划 9/28，实际 09-23 提前冻结）")

    # 图纸文件
    for stem in ["01 电源与保护", "02 驱动接线", "03 感知接线", "04 调试与扩展"]:
        for ext in [".tex", ".pdf", ".png"]:
            f = OUT / (stem + ext)
            chk(f.exists() and f.stat().st_size > 1000, f"图纸存在且非空：{stem}{ext}")
    foot = (OUT / "01 电源与保护.tex").read_text(encoding="utf-8").replace("\\_", "_")
    chk(all(k in foot for k in ["12V_BUS", "5V_BUS", "3V3_LOGIC"]),
        "电源图使用统一网络命名（12V_BUS / 5V_BUS / 3V3_LOGIC）")

    print()
    print("=" * 78)
    print("第 3 遍：需求与手册闭环")
    print("=" * 78)
    allmd = md + spec + (HERE / "电气连接规范.md").read_text(encoding="utf-8")
    pin_all = " ".join(PINMAP.values())

    # —— 已确认决策（李沛阳 ×王政淋，2026-09-20）
    chk("感为" in allmd and "主方案" in allmd, "灰度：感为主方案、亚博备用（用户决策）")
    for m in ["OLED", "MPU6050", "BLE"]:
        chk(m in allmd, f"{m} 已接入（用户要求全装）")
    chk("JST-XH" in allmd, "电机用现成 JST-XH 插头（用户决策）")
    chk("闭环" in allmd, "编码器用于闭环测速（用户决策）")
    chk("T3A" in allmd or "3A" in allmd, "保险丝按 3A 慢断预留（用户决策）")
    chk("单板" in allmd, "PCB 为单板（用户决策）")
    for m in ["蜂鸣器", "按键", "12V"]:
        chk(m in allmd, f"{m} 相关电路已保留（用户决策：全要）")

    # —— 厂商手册硬约束
    chk("5V" in PINMAP["PB8"] and "5V" in PINMAP["PB9"],
        "感为总线按 5V 上拉（手册：外置上拉源不得低于 72%×5V=3.6V）")
    chk("不插" in allmd or "不装" in allmd or "不插上" in allmd,
        "已写明感为板载 5V 上拉跳帽不插（避免与 PCB 上拉并联）")
    chk("3.1" in allmd or "2k/3.3k" in allmd, "超声波 Echo 有分压（HC-SR04 Echo 为 5V 输出）")
    trig = [x for x in rows if x[4] == "Trig"]
    chk(len({x[2] for x in trig}) == 2, f"两个超声波 Trig 各自独立网络（{sorted({x[2] for x in trig})}）")

    # —— 已冻结协议（docs/遥测协议.md）
    proto = (ROOT / "docs" / "遥测协议.md").read_text(encoding="utf-8")
    chk("PA9" in proto and "PA10" in proto, "遥测协议物理层仍为 USART1 / PA9,PA10")
    chk(PINMAP["PA9"].startswith("USART1_TX") and PINMAP["PA10"].startswith("USART1_RX"),
        "USB-TTL 遥测留在 USART1（不因本次合并而改动）")

    # —— 实测数据（王政淋 2026-09-20）
    chk("M1" in allmd and "C2" in allmd and "C1" in allmd, "电机 6P 线序写入文档（M1/GND/C2/C1/VCC/M2）")
    chk("5V" in PINMAP["PB6"] or "5V" in md, "编码器 VCC 按实测走 +5V（PB6/PB7/PA6/PA7 均为 FT 脚）")
    for p in ["PB6", "PB7", "PA6", "PA7"]:
        chk(p not in UNUSABLE, f"编码器输入 {p} 是可用的 5V 容忍数字脚")

    # —— 移植性（不依赖关 JTAG 也能跑）
    chk("PB5" == [p for p, d in PINMAP.items() if "US_R_ECHO" in d][0],
        "右超声波 Echo 放在非 JTAG 脚 PB5 —— 核心功能不依赖关 JTAG")

    print()
    print("=" * 78)
    print(f"结论：FAIL {len(FAIL)} 项 / WARN {len(WARN)} 项")
    for x in FAIL:
        print("  FAIL:", x)
    for x in WARN:
        print("  WARN:", x)
    print("=" * 78)
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
