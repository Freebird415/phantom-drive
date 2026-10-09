# -*- coding: utf-8 -*-
"""生成《任务清单》的 Markdown 与 PDF（同一份数据）。

唯一事实来源：本文件下面的数据表。改完重跑：

    python 生成任务清单.py --build     # 生成 任务清单.md + 任务清单.pdf
    python 生成任务清单.py              # 只生成 任务清单.md / 任务清单.tex

输出（同目录）：
    任务清单.md     与 PDF 同内容的纯文本版（方便贴群、diff、被 agent 读）
    任务清单.tex    LaTeX 源（由本脚本生成，勿手改）
    任务清单.pdf    对外分发的排版版

说明：PDF 在 ASCII 临时目录里用 xelatex 编译（Windows 中文路径下 xelatex 不稳）。
数据与 `项目记录/开发记录.md` 同步；如有冲突，以《开发记录》为准。
"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
MIK = Path(r"C:\Users\35276\AppData\Local\Programs\MiKTeX\miktex\bin\x64")

TITLE = "工创小车 · 任务清单"
SUBTITLE = "重走古丝路 · 自动循迹避障小车（华南理工大学 · 工程训练分散项目）"
META = "5 人 · 13 项任务 · v2.5 · 更新：2026-10-10"
LEAD = ("本清单只讲两件事：**已经完成了什么**、**接下来谁做什么**。"
        "与 `项目记录/开发记录.md` 同步；如两份说法不一致，以《开发记录》为准。所有截止时间统一用日期表示。")

# ---------------------------------------------------------------------------
# 0. 关键时间与红线
# ---------------------------------------------------------------------------
KEY_TIME = [
    ("现在", "10/10（周六）· **补周三课的分散实训日，全组在场至少半天**；"
             "成车已装 + **新 PCB v2.1 验收正常**；✅ BLE 识别故障已闭环（原核心板 3V3 故障，换板修复）；"
             "**当前第一优先：探头 15° 新几何标测 + 循迹弯道调参**"),
    ("内部全功能冻结", "10/25（周日）—— 4 障全过 + 精停 + 全套材料（节点小幅前移，见 §5）"),
    ("预测比赛周", "10/26~11/1（比赛 + 演讲）"),
    ("已过去", "中秋 9/25~9/27、国庆 10/1~10/7（假期只排软件类任务，已过）"),
]

RED_LINES = [
    "整车尺寸不超过 250×200×200mm（超标每轮扣 10%）",
    "全车只能一块电源（多电源扣 10%）",
    "车架必须自制，主结构自己设计 + 中心设备加工 + 全程留照片/视频（买现成一票否决）",
    "全自动运行，比赛禁遥控（⇒ 无线链路只能收日志，不能发命令）",
    "避障不得接触障碍物；与障碍物碰一下即该障不得分",
    "终点停车：车尾投影不得超过终点线（20 分）",
]

# ---------------------------------------------------------------------------
# 1. 编号规则
# ---------------------------------------------------------------------------
NUM_RULE = [
    ("M", "机械", "M01 结构设计与建模 · M02 加工与装配"),
    ("H", "硬件", "H01 选型与采购 · H02 电路与电气集成"),
    ("A", "算法", "A01 循迹控制 · A02 避障与回线"),
    ("P", "平台（软件基础）", "P01 反馈链路与工具链 · P02 底层驱动与框架"),
    ("D", "文档与协作", "D01 项目仓库与协作规范 · D02 文档与展示材料"),
    ("F", "财务", "F01 账务与报销"),
    ("S", "辅助", "S01 辅助电路 · S02 辅助装配"),
]

# ---------------------------------------------------------------------------
# 2. 分工总览
# ---------------------------------------------------------------------------
ASSIGN = [
    ("李沛阳（组长）", "D01 仓库与协作规范 · P01 反馈链路与工具链 · P02 底层驱动与框架 · F01 账务与报销"),
    ("王政淋", "H01 选型与采购 · H02 电路与电气集成"),
    ("熊雅琦", "A01 循迹控制 · S01 辅助电路 · S02 辅助装配"),
    ("李昕程", "M01 结构设计与建模 · M02 加工与装配"),
    ("毕烨恺", "A02 避障与回线 · D02 文档与展示材料"),
    ("一起做", "车架与 PCB 的尺寸与孔位（王政淋 + 李昕程当面商定，熊雅琦辅助核对）"),
]

# ---------------------------------------------------------------------------
# 3. 任务详情（按人）—— 只列接下来要做的
#    行：（编号，任务，要做什么，交给谁什么，需要谁的什么，截止日期）
# ---------------------------------------------------------------------------
MEMBERS = [
    {
        "name": "李沛阳（组长）—— P01 · P02 · F01",
        "rows": [
            ("P01", "反馈链路与工具链",
             "接真实传感器，读数落盘出图；再做 PID/状态机数据与在线调参全链路；板载快照（脱机记录）",
             "熊雅琦、毕烨恺调试时直接用的一整套工具（含跑车脱机数据读取）",
             "李昕程的实车 v1（已能跑）后跑全链路",
             "阶段2 9/30 ✅；阶段3 目标 10/12（参数掉电保存、绝对计数锚帧待做）"),
            ("P02", "底层驱动与框架",
             "驱动接口初版已交（9/30）；后续补速度换算/电压采样/零偏标定，**驱动接口冻结目标 10/12**",
             "熊雅琦、毕烨恺的驱动接口说明（函数怎么调、数据长什么样）",
             "《引脚分配表》（v3.0 已 9/23 冻结）",
             "初版 9/30 ✅；冻结目标 10/12"),
            ("F01", "账务与报销",
             "收集发票与商品照片、填成本台账、赛后统一报账",
             "毕烨恺的成本台账（进 PPT）",
             "全组的电子发票 + 收货照片 + 一句用途说明（10/18 前收齐）",
             "收齐 10/18；赛后一周内报账"),
        ],
    },
    {
        "name": "王政淋 —— H01 · H02",
        "rows": [
            ("H01", "选型与采购",
             "电子元件到货清点、缺件补单",
             "全组齐套可用的电子元件",
             "",
             "清点 9/30"),
            ("H02", "电路与电气集成",
             "校验接线设计 → 落原理图 → 布板下单 → 打板焊接 → 点亮并交接线图",
             "李沛阳的接线图与点亮记录",
             "李沛阳的《引脚分配表》（v3.0 已冻结）；与李昕程当面商定的板形与孔位",
             "原理图 9/24 ✅；下单 9/30 ✅；**PCB v2.1 已到货装车、电器功能验收正常（10/10）**；点亮记录与接线图待整理入库"),
        ],
    },
    {
        "name": "熊雅琦 —— A01 · S01 · S02",
        "rows": [
            ("A01", "循迹控制",
             "灰度处理 + PID + 丢线处理 + 终点精停；本人上车调参；目标全赛道稳定",
             "毕烨恺的全赛道稳定循迹版（他做避障回线的基座）",
             "李沛阳的驱动接口与板载快照（跑车数据事后读）；李昕程的实车（已能跑）",
             "**10/1 已跑通黑胶带轨道 ✅**；算法已到 v8；能跑弯 10/13；全赛道稳定 10/18"),
            ("S01", "辅助电路",
             "打板前核对元件、封装与接线，避免打板出错",
             "王政淋一份核对过的落图依据",
             "王政淋的 `netlist.json` 与电气设计",
             "✅ 已完成（随电气设计 9/23 冻结）"),
            ("S02", "辅助装配",
             "装配打下手、拍照记录、固定走线",
             "李昕程的装配照片与记录",
             "李昕程的装配安排与到货材料",
             "随 M02 展开；定版目标 10/17"),
        ],
    },
    {
        "name": "李昕程 —— M01 · M02",
        "rows": [
            ("M01", "结构设计与建模",
             "整车模型、关键尺寸、关键孔位、加工图；导出件与建模源文件传 `mech/设计文件/`",
             "王政淋的板形依据、M02 的加工图、全组的尺寸确认（红线 ≤250×200×200mm）",
             "王政淋的板子需求（当面商定）和电控件尺寸",
             "模型冻结 9/28 ✅；**待补**：工程图 PDF 与《尺寸与孔位》"),
            ("M02", "加工与装配",
             "用中心设备加工并全程拍照录像；装出实车，**定版目标 10/17**",
             "熊雅琦、毕烨恺的实车；毕烨恺的加工证据",
             "自己的加工图；王政淋的点亮结果用于装电控",
             "**加工已开始（9/30 证据入库）✅**；实车已能跑（10/1）；定版目标 10/17"),
        ],
    },
    {
        "name": "毕烨恺 —— A02 · D02",
        "rows": [
            ("A02", "避障与回线",
             "超声波避障：单测 → 检测 demo → 绕行 + 回线 → 与循迹/精停融合；本人上车调参",
             "全组的成品功能：4 障全过 + 精停",
             "李沛阳的遥测与驱动接口（已交）；超声波实测读数（-2 = 前方空旷）；**探头已换左右各外掰 15° ⇒ 先标测新几何**；熊雅琦的稳定循迹版（10/18）",
             "⚠️ 先标测 15° 几何；单测 10/15；demo 10/17；绕行回线 10/21；达标 10/22"),
            ("D02", "文档与展示材料",
             "做 PPT、答辩问答、演示视频；收全员素材",
             "全组一套完整的展示材料",
             "全组素材（10/17 前）；李沛阳的成本台账（10/18）；李昕程的加工证据（加工期间）",
             "草稿 10/18；冻结 10/25"),
        ],
    },
]

# 共同任务（不是新增编号，属于 M01 与 H02 的交接点）
JOINT = {
    "name": "车架与 PCB 的尺寸与孔位（王政淋 + 李昕程，熊雅琦辅助核对）",
    "do": "板子做多大、装在底板哪里、打几个孔、线从哪边出来、上面留多高——**两人当面商定即可，不用写文档**。",
    "give": "王政淋画 PCB 的尺寸依据、李昕程装车的同一份尺寸",
    "need": "熊雅琦帮忙核对元件位置",
    "ddl": "9/24 前对齐；之后要改，两人说一声即可",
}

# ---------------------------------------------------------------------------
# 4. 交接时间线（按节点分组，不逐日罗列）
# ---------------------------------------------------------------------------
TIMELINE = [
    {
        "period": "9/21~9/27 · 原理图与板块",
        "events": [
            "**9/24**：王政淋 落定原理图（板形/孔位与李昕程当面商定）",
            "**9/24**：李昕程 交设计文件（整车模型进入 `mech/设计文件/`）",
            "**9/23（实际提前）**：电气设计 v3.0 与《引脚分配表》冻结；八路灰度/编码器/MPU6050 逐个 bring-up 验收",
            "**9/28 前**：李昕程 自行下单加工材料与紧固件",
        ],
    },
    {
        "period": "9/28~9/30 · 关键下单窗口",
        "events": [
            "**9/28**：《引脚分配表》**已于 9/23 提前冻结**；李昕程 模型冻结；熊雅琦 完成元件与封装核对",
            "**9/30**：王政淋 PCB 下单 ＋ 电子元件到货清点；李沛阳 交驱动接口初版 ✅",
            "**9/30（实际）**：**PCB 提前到货并装出成车**，固件 33 脚与 v3.0 逐脚一致 ⇒ 验收改为在成车上逐模块做",
        ],
    },
    {
        "period": "10/1~10/7 · 国庆假（硬件停）",
        "events": [
            "**10/1（实际）**：车首次沿黑胶带跑通循迹；板载快照全链路打通（脱机跑→掉电不丢→事后读→出图）",
            "熊雅琦：继续调循迹（算法迭代到 v6/v7/v8）；毕烨恺：离线写避障算法代码",
            "李沛阳：无线日志工具链（网页界面 + 监测模式）、驱动逐个冒烟、P02 补缺项",
        ],
    },
    {
        "period": "10/8~10/10 · 节后硬件攻坚（含 10/10 集中实训）",
        "events": [
            "**10/8~10/10**：李昕程 中心设备加工车架与底板（全程拍照录像）—— 实际 9/30 已开工",
            "**10/10（补周三课 · 统一分散实训）**：全组到场至少半天，集中攻坚 —— 探头 **15° 新几何标测**、循迹弯道调参、驱动接口冻结前收尾（清单见 `docs/agent对接.md` §三·补三）；原定的“攻 BLE 识别”已于当晚闭环 ✓",
            "**10/10（实际）**：**新 PCB v2.1 到货并装车、电器功能验收正常**；王政淋 点亮记录/接线图待入库",
        ],
    },
    {
        "period": "10/11~10/18 · 上车调试周",
        "events": [
            "**10/12**：李沛阳 交驱动接口冻结版 ＋ 遥测上车（原 10/14 提前）",
            "**10/13**：熊雅琦 循迹能跑弯；两人开始上车调参",
            "**10/15**：毕烨恺 避障单测完成（含 15° 几何标测）",
            "**10/17**：李昕程 交定版车架；全组素材 → 毕烨恺",
            "**10/18**：熊雅琦 交稳定循迹版（全赛道）→ 毕烨恺；李沛阳 交成本台账",
        ],
    },
    {
        "period": "10/19~10/25 · 收口周",
        "events": [
            "**10/21**：毕烨恺 绕行＋回线＋与循迹/精停融合",
            "**10/22**：毕烨恺 避障达标：4 障全过 ＋ 精停",
            "**10/25**：全套材料冻结 ＋ 赛前完整演练（避障达标与材料均已完成一轮）",
        ],
    },
    {
        "period": "10/26~11/1 · 比赛周",
        "events": [
            "全组：比赛 ＋ 演讲",
        ],
    },
]

# ---------------------------------------------------------------------------
# 5. 里程碑
# ---------------------------------------------------------------------------
MILESTONES = [
    ("9/24（周四）", "原理图定稿 ＋ 板形/孔位当面商定；设计文件交付"),
    ("9/28（周一）", "面包板 demo 交付并开始冒烟；《引脚分配表》冻结；车架模型冻结"),
    ("9/30（周三）", "PCB 下单（节前窗口）＋ 驱动接口初版"),
    ("10/10（周六 · 补课日）", "PCB v2.1 装车验收；**3V3 故障修复 → BLE 恢复**；探头 15° 几何标测"),
    ("10/11（周日）", "PCB 点亮记录 ＋ 接线图入库"),
    ("10/12（周一）", "驱动接口冻结；遥测上车"),
    ("10/13（周二）", "循迹能跑弯"),
    ("10/15（周四）", "避障单测完成（含 15° 几何标测）"),
    ("10/17（周六）", "定版车架；全员素材交齐"),
    ("10/18（周日）", "稳定循迹全赛道；成本台账；展示材料草稿"),
    ("10/22（周四）", "避障达标：4 障全过 ＋ 精停"),
    ("10/25（周日）", "**全功能冻结**：全套材料 ＋ 赛前完整演练"),
    ("10/26~11/1", "比赛 ＋ 演讲"),
]

# ---------------------------------------------------------------------------
# 6. 假期安排
# ---------------------------------------------------------------------------
HOLIDAYS = [
    ("9/25~9/27", "中秋节",
     "人在校，照常推进；王政淋按原理图搭面包板 demo。"),
    ("10/1~10/7", "国庆节",
     "实验室和快递基本停。只排软件任务：驱动冒烟、P02、离线算法、文档与素材；硬件一律等 10/8 以后。"),
]

# ---------------------------------------------------------------------------
# 7. 协作纪律
# ---------------------------------------------------------------------------
DISCIPLINE = [
    "固定顺序：拉取 → 放入 → 提交。开工前双击 `拉取.bat`，干完双击 `提交.bat`；提交要组长确认。",
    "掐好节前窗口：PCB 下单 9/30 前（王政淋）、加工材料下单 9/24 前（李昕程）；已将 PCB 下单（9/30）与加工材料下单（9/24）完成 ✓。",
    "⭐ **当前第一优先：探头 15° 新几何标测**（上场前花 5 分钟量“首次检出距离”，别拿“预期几何”当已知事实）。",
    "⭐ BLE 识别故障已闭环（2026-10-10）：真因 = 原核心板 3V3 网络故障，换板后 3V3=3.3V、无线日志恢复；仍保留板载快照（`DUMP`）作为回退。",
    "板形/孔位与接口朝向定下后要改，两人说一声（《引脚分配表》同）；驱动接口 10/12 冻结后只加功能不改接口。",
    "算法谁写的谁上车调，调参不外包；每次上车测试留日志 + 一句话结论（双击「蓝牙日志.bat」，导出片段即带 `note.md` 摘要）。",
    "判罚、评分类问题先查 `参考资料/原文件/` 原文，不确定就在群里问，别自己猜。",
]

# ---------------------------------------------------------------------------
# 8. 已完成（截至 10/10）
# ---------------------------------------------------------------------------
COMPLETED = [
    ("D01 仓库与协作规范",
     "仓库骨架、三个一键脚本（拉取/提交/面板）、说明书、练习场、各工作目录 README 就绪；5 人全部入仓"),
    ("P01 阶段 1",
     "一键烧录 ＋ 100Hz 遥测 ＋ 自动存档出图 ＋ 在线调参命令（9/13 跑通）"),
    ("引脚与电气设计 v3.0",
     "`docs/引脚分配表.md`（**9/23 提前冻结**）、`hardware/电气设计/`（4 张图 ＋《电气连接规范》＋ `完整接线.md` ＋ `netlist.json` ＋ 可执行 `自检.py`，106 项全过）"),
    ("P02 驱动与接口初版",
     "8 个器件驱动全部实机验收；`firmware/驱动接口说明.md` ＋ 算法层代码缝（A01 `app_trace.c` / A02 `app_avoid.c`，默认只算不发）"),
    ("P01 阶段 2 · 板载快照",
     "`$T` 接真实灰度；**板载快照全链路**（RAM 记录 → 停车写 Flash → 掉电不丢 → `tools/snapshot.py` 读出图）；`track.py` 重建实际轨迹"),
    ("P01 · 无线日志工具链（网页界面）",
     "双击「蓝牙日志.bat」→ 浏览器界面实时看帧；**监测模式**（只显示不写文件 + 空格一键记录 + 最近 ~65s 回溯入库）；拖入 CSV 导入；按键导出片段（自动带 `derived.csv` + `note.md` 摘要）；超声波两列 + 存在性保证"),
    ("H01 选型与采购",
     "BOM 与器件资料索引定稿；万向轮、编码器座、HC-SR04、电源座等已下单；电机确认带编码器；避障定案 HC-SR04 ×2；**PCB v2.1 到货并装车（10/10）**"),
    ("H02 电气集成",
     "原理图落定、下单到位；**PCB v2.1 已装车、电器功能验收正常（10/10）**（MPU6050 / 灰度 / 超声波 ×2 / 按键 / 蜂鸣器 / TB6612 逻辑；OLED 已放弃）；固件 33 脚与 v3.0 逐脚一致；**电气文档已按实物原理图逐针校正（10/10 深夜，E-34）**"),
    ("A01 循迹（含算法迭代）",
     "**车首次沿黑胶带跑通（10/1）**；灰度加权偏差 + PID + 丢线处理 + **终点精停**（全黑判定 → 前进 D mm → 刹车）＋ `DISTCAL`/`STOPDIST` 标定；**算法迭代到 v8**：v6 修“精停从未触发”、v7 弯道降速改接陀螺仪、v8 精停两道闸（窄线武装 + 800mm 里程门槛）"),
    ("A02 避障与回线 · 准备部分",
     "代码缝与接口就绪（默认只决策不发车）；超声波驱动实机验收（`-2` = 前方空旷）、`$U`/`$E` 已上蓝牙；**探头换装左右各外掰 15°**（无中央盲区；新几何待标测）"),
    ("M01/M02 建模与加工",
     "三轮构型整车模型与截图、整车 STEP ＋ 底板 STL；模型冻结 9/28；激光切割/3D 打印/装配证据入库"),
    ("资料与协作",
     "参考资料补齐（手册、实物照片、器件针序）；计划日期化并建任务清单生成器；`mech/设计文件/`、`加工证据/` 就位"),
]


# ===========================================================================
#  下面是生成逻辑，一般不用改
# ===========================================================================
def _tex_plain(s: str) -> str:
    for a, b in [("\\", r"\textbackslash{}"), ("&", r"\&"), ("%", r"\%"), ("#", r"\#"),
                 ("_", r"\_"), ("$", r"\$"), ("{", r"\{"), ("}", r"\}"),
                 ("~", r"\textasciitilde{}"), ("^", r"\textasciicircum{}"),
                 ("≤", r"$\leq$ "), ("−", "-"), ("≥", r"$\geq$ ")]:
        s = s.replace(a, b)
    return s


def tex_escape(s: str) -> str:
    """转义 LaTeX 特殊字符；`xxx` 渲染成等宽代码体，**xxx** 渲染成粗体。"""
    out = []
    for i, part in enumerate(s.split("`")):
        out.append(r"\texttt{" + _tex_plain(part) + "}" if i % 2 else _tex_plain(part))
    s = "".join(out)
    return re.sub(r"\*\*(.+?)\*\*", r"\\textbf{\1}", s)


def row_cell(r) -> str:
    no, name, do, give, need, ddl = r
    parts = [f"做：{do}"]
    if give:
        parts.append(f"交：{give}")
    if need:
        parts.append(f"需：{need}")
    return "；".join(parts)


def emit_md() -> str:
    L: list[str] = []
    L.append(f"# {TITLE}（v2.5）")
    L.append("")
    L.append(f"> {SUBTITLE}")
    L.append(f"> {META}")
    L.append(">")
    L.append(f"> {LEAD}")
    L.append(">")
    L.append("> 本文件由 `生成任务清单.py` 生成（md 与 pdf 同源，重跑 `--build` 一起更新）。")
    L.append("")
    L.append("## 0. 关键时间与红线")
    L.append("")
    L.append("| 项 | 内容 |")
    L.append("|---|---|")
    for k, v in KEY_TIME:
        L.append(f"| {k} | {v} |")
    L.append("")
    L.append("**红线（违反要扣分或没资格）**")
    L.append("")
    for r in RED_LINES:
        L.append(f"- {r}")
    L.append("")
    L.append("## 1. 编号规则")
    L.append("")
    L.append("任务号 = 一个字母 + 两位数字：字母看类别，数字从 01 起。")
    L.append("")
    L.append("| 字母 | 类别 | 任务 |")
    L.append("|---|---|---|")
    for a, b, c in NUM_RULE:
        L.append(f"| {a} | {b} | {c} |")
    L.append("")
    L.append("## 2. 分工总览")
    L.append("")
    L.append("| 成员 | 负责任务 |")
    L.append("|---|---|")
    for a, b in ASSIGN:
        L.append(f"| {a} | {b} |")
    L.append("")
    L.append("## 3. 任务详情（接下来要做的）")
    L.append("")
    L.append("每行读法：**做**什么 → **交**给谁什么 → **需**要谁的什么 → 什么时候要。已完成的事在文末 §8。")
    for m in MEMBERS:
        L.append("")
        L.append(f"### {m['name']}")
        L.append("")
        L.append("| 编号 | 任务 | 做 · 交 · 需 | 截止日期 |")
        L.append("|---|---|---|---|")
        for r in m["rows"]:
            L.append(f"| {r[0]} | {r[1]} | {row_cell(r)} | {r[5]} |")
    L.append("")
    L.append(f"### 共同任务：{JOINT['name']}")
    L.append("")
    L.append(f"{JOINT['do']}")
    L.append("")
    L.append(f"**交：**{JOINT['give']}　**需：**{JOINT['need']}")
    L.append("")
    L.append(f"**截止：**{JOINT['ddl']}")
    L.append("")
    L.append("## 4. 交接时间线（按节点）")
    L.append("")
    L.append("只列重要交接日；按周与节假日分断。")
    for g in TIMELINE:
        L.append("")
        L.append(f"### {g['period']}")
        L.append("")
        for e in g["events"]:
            L.append(f"- {e}")
    L.append("")
    L.append("## 5. 里程碑")
    L.append("")
    L.append("| 日期 | 要做完什么 |")
    L.append("|---|---|")
    for a, b in MILESTONES:
        L.append(f"| {a} | {b} |")
    L.append("")
    L.append("## 6. 假期安排")
    L.append("")
    L.append("| 时间 | 假期 | 怎么安排 |")
    L.append("|---|---|---|")
    for a, b, c in HOLIDAYS:
        L.append(f"| {a} | {b} | {c} |")
    L.append("")
    L.append("## 7. 协作纪律")
    L.append("")
    for i, d in enumerate(DISCIPLINE, 1):
        L.append(f"{i}. {d}")
    L.append("")
    L.append("## 8. 已完成（截至 10/10）")
    L.append("")
    L.append("| 事项 | 结果 |")
    L.append("|---|---|")
    for a, b in COMPLETED:
        L.append(f"| {a} | {b} |")
    L.append("")
    return "\n".join(L)


def emit_tex() -> str:
    L: list[str] = []
    A = L.append
    A(r"\documentclass[10.5pt]{article}")
    A(r"\usepackage[UTF8]{ctex}")
    A(r"\usepackage[a4paper,top=1.5cm,bottom=1.5cm,left=1.4cm,right=1.4cm]{geometry}")
    A(r"\usepackage{booktabs}")
    A(r"\usepackage{longtable}")
    A(r"\usepackage{array}")
    A(r"\usepackage{xcolor}")
    A(r"\definecolor{accent}{HTML}{1F4E79}")
    A(r"\newcommand{\sect}[1]{\par\vspace{9pt}{\large\bfseries\color{accent}#1}\par\vspace{3pt}}")
    A(r"\newcolumntype{L}[1]{>{\raggedright\arraybackslash}p{#1}}")
    A(r"\setlength{\parindent}{0pt}")
    A(r"\setlength{\parskip}{2pt}")
    A(r"\begin{document}")
    A(r"\begin{center}")
    A(r"{\LARGE\bfseries " + tex_escape(TITLE) + r"}\par\vspace{4pt}")
    A(r"{\small " + tex_escape(SUBTITLE) + r"}\par\vspace{2pt}")
    A(r"{\small " + tex_escape(META) + r"}\par")
    A(r"\end{center}")
    A(r"\vspace{-2pt}")
    A("")

    A(r"\sect{0. 关键时间与红线}")
    A(r"\begin{tabular}{@{}L{2.6cm}L{14.4cm}@{}}")
    A(r"\toprule")
    A(r"\textbf{项} & \textbf{内容}\tabularnewline")
    A(r"\midrule")
    for k, v in KEY_TIME:
        A(tex_escape(k) + r" & " + tex_escape(v) + r"\tabularnewline")
    A(r"\bottomrule")
    A(r"\end{tabular}")
    A("")
    A(r"\par\vspace{4pt}")
    A(r"{\small \textbf{红线（违反要扣分或没资格）}：" + tex_escape("；".join(RED_LINES)) + r"}\par")
    A("")

    A(r"\sect{1. 编号规则}")
    A(r"任务号 = 一个字母 + 两位数字；字母看类别，数字从 01 起。\par\vspace{3pt}")
    A(r"\begin{tabular}{@{}L{1.2cm}L{4.2cm}L{11.6cm}@{}}")
    A(r"\toprule")
    A(r"\textbf{字母} & \textbf{类别} & \textbf{任务}\tabularnewline")
    A(r"\midrule")
    for a, b, c in NUM_RULE:
        A(tex_escape(a) + r" & " + tex_escape(b) + r" & " + tex_escape(c) + r"\tabularnewline")
    A(r"\bottomrule")
    A(r"\end{tabular}")
    A("")

    A(r"\sect{2. 分工总览}")
    A(r"\begin{tabular}{@{}L{3.4cm}L{13.6cm}@{}}")
    A(r"\toprule")
    A(r"\textbf{成员} & \textbf{负责任务}\tabularnewline")
    A(r"\midrule")
    for a, b in ASSIGN:
        A(tex_escape(a) + r" & " + tex_escape(b) + r"\tabularnewline")
    A(r"\bottomrule")
    A(r"\end{tabular}")
    A("")

    A(r"\sect{3. 任务详情（接下来要做的）}")
    A(r"{\small 每行读法：做 什么 → 交 给谁什么 → 需 要谁的什么 → 什么时候要。已完成的事在文末 §8。\par}")
    for m in MEMBERS:
        A(r"\vspace{5pt}{\bfseries " + tex_escape(m["name"]) + r"}\par\vspace{2pt}")
        A(r"{\small")
        A(r"\begin{longtable}{@{}L{1.0cm}L{2.3cm}L{9.7cm}L{4.8cm}@{}}")
        A(r"\toprule")
        A(r"\textbf{编号} & \textbf{任务} & \textbf{做 · 交 · 需} & \textbf{截止日期}\tabularnewline")
        A(r"\midrule")
        A(r"\endhead")
        for r in m["rows"]:
            A(tex_escape(r[0]) + r" & " + tex_escape(r[1]) + r" & " + tex_escape(row_cell(r)) + r" & " + tex_escape(r[5]) + r"\tabularnewline")
        A(r"\bottomrule")
        A(r"\end{longtable}")
        A("")
        A(r"}")
        A("")
    A(r"\vspace{6pt}{\bfseries 共同任务：" + tex_escape(JOINT["name"]) + r"}\par\vspace{3pt}")
    A(tex_escape(JOINT["do"]) + r"\par\vspace{2pt}")
    A(r"\textbf{交：}" + tex_escape(JOINT["give"]) + r"　\textbf{需：}" + tex_escape(JOINT["need"]) + r"\par\vspace{2pt}")
    A(r"\textbf{截止：}" + tex_escape(JOINT["ddl"]) + r"\par")
    A("")

    A(r"\sect{4. 交接时间线（按节点）}")
    A(r"{\small 只列重要交接日；按周与节假日分断。\par}")
    for g in TIMELINE:
        A(r"\vspace{5pt}{\bfseries\color{accent}" + tex_escape(g["period"]) + r"}\par\vspace{2pt}")
        A(r"\begin{itemize}\setlength{\itemsep}{1pt}\setlength{\parskip}{0pt}")
        for e in g["events"]:
            A(r"\item " + tex_escape(e))
        A(r"\end{itemize}")
    A("")

    A(r"\newpage")
    A(r"\sect{5. 里程碑}")
    A(r"{\small")
    A(r"\begin{tabular}{@{}L{2.6cm}L{14.4cm}@{}}")
    A(r"\toprule")
    A(r"\textbf{日期} & \textbf{要做完什么}\tabularnewline")
    A(r"\midrule")
    for a, b in MILESTONES:
        A(tex_escape(a) + r" & " + tex_escape(b) + r"\tabularnewline")
    A(r"\bottomrule")
    A(r"\end{tabular}")
    A("")
    A(r"}")
    A("")

    A(r"\sect{6. 假期安排}")
    A(r"{\small")
    A(r"\begin{tabular}{@{}L{2.6cm}L{2.2cm}L{12.2cm}@{}}")
    A(r"\toprule")
    A(r"\textbf{时间} & \textbf{假期} & \textbf{怎么安排}\tabularnewline")
    A(r"\midrule")
    for a, b, c in HOLIDAYS:
        A(tex_escape(a) + r" & " + tex_escape(b) + r" & " + tex_escape(c) + r"\tabularnewline")
    A(r"\bottomrule")
    A(r"\end{tabular}")
    A("")
    A(r"}")
    A("")

    A(r"\sect{7. 协作纪律}")
    A(r"\begin{enumerate}\setlength{\itemsep}{2pt}")
    for d in DISCIPLINE:
        A(r"\item " + tex_escape(d))
    A(r"\end{enumerate}")
    A("")

    A(r"\sect{8. 已完成（截至 10/10）}")
    A(r"{\small")
    A(r"\begin{tabular}{@{}L{3.6cm}L{14.2cm}@{}}")
    A(r"\toprule")
    A(r"\textbf{事项} & \textbf{结果}\tabularnewline")
    A(r"\midrule")
    for a, b in COMPLETED:
        A(tex_escape(a) + r" & " + tex_escape(b) + r"\tabularnewline")
    A(r"\bottomrule")
    A(r"\end{tabular}")
    A("")
    A(r"}")
    A("")

    A(r"\end{document}")
    return "\n".join(L) + "\n"


def build_pdf() -> bool:
    tmp = Path(tempfile.mkdtemp(prefix="pd_plan_"))
    shutil.copy2(HERE / "任务清单.tex", tmp / "plan.tex")
    for _ in range(2):
        r = subprocess.run([str(MIK / "xelatex.exe"), "-interaction=nonstopmode", "-halt-on-error", "plan.tex"],
                           cwd=tmp, capture_output=True)
    if not (tmp / "plan.pdf").exists():
        print("[FAIL] xelatex 编译失败，最后 1500 字符日志：")
        log = tmp / "plan.log"
        print(log.read_text(errors="replace")[-1500:] if log.exists() else r.stdout.decode(errors="replace")[-1500:])
        shutil.rmtree(tmp, ignore_errors=True)
        return False
    shutil.copy2(tmp / "plan.pdf", HERE / "任务清单.pdf")
    shutil.rmtree(tmp, ignore_errors=True)
    print("[OK] 任务清单.pdf")
    return True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", action="store_true", help="同时编译 PDF")
    args = ap.parse_args()
    (HERE / "任务清单.md").write_text(emit_md(), encoding="utf-8")
    (HERE / "任务清单.tex").write_text(emit_tex(), encoding="utf-8")
    print("[OK] 任务清单.md / 任务清单.tex")
    if args.build:
        return 0 if build_pdf() else 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
