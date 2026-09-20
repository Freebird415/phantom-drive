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
META = "5 人 · 13 项任务 · v2.3 · 更新：2026-09-20（第 3 周末）"
LEAD = ("本清单只讲两件事：**已经完成了什么**、**接下来谁做什么**。"
        "与 `项目记录/开发记录.md` 同步；如两份说法不一致，以《开发记录》为准。所有截止时间统一用日期表示。")

# ---------------------------------------------------------------------------
# 0. 关键时间与红线
# ---------------------------------------------------------------------------
KEY_TIME = [
    ("现在", "9/20（周日）· 第 3 周末"),
    ("中秋节", "9/25~9/27（人在校，照常推进）"),
    ("国庆放假", "10/1~10/7（只排软件类任务）"),
    ("内部全功能冻结", "10/25（周日）—— 4 障全过 + 精停 + 全套材料"),
    ("预测比赛周", "10/26~11/1（比赛 + 演讲）"),
]

RED_LINES = [
    "整车尺寸不超过 250×200×200mm（超标每轮扣 10%）",
    "全车只能一块电源（多电源扣 10%）",
    "车架必须自制，主结构自己设计 + 中心设备加工 + 全程留照片/视频（买现成一票否决）",
    "全自动运行，比赛禁遥控",
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
    ("一起做", "车架与 PCB 对接约定（王政淋 + 李昕程，熊雅琦辅助）"),
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
             "接真实传感器，读数落盘出图；再做 PID/状态机数据与在线调参全链路",
             "熊雅琦、毕烨恺调试时直接用的一整套工具",
             "王政淋的面包板 demo（9/28）先接真实模块；李昕程的实车 v1（10/13）后跑全链路",
             "阶段2 9/30；阶段3 10/14"),
            ("P02", "底层驱动与框架",
             "写电机 PWM、编码器测速、串口、I2C、超声波、电压检测；9/30 出初版，先在面包板 demo 上冒烟，10/14 冻结",
             "熊雅琦、毕烨恺的驱动接口说明（函数怎么调、数据长什么样）",
             "王政淋的面包板 demo（9/28）；王政淋校验后的《引脚分配表》（9/28 冻结）",
             "初版 9/30；冻结 10/14"),
            ("F01", "账务与报销",
             "收集发票与商品照片、填成本台账、赛后统一报账",
             "毕烨恺的成本台账（进 PPT）",
             "全组的电子发票 + 收货照片 + 一句用途说明（10/21 前收齐）",
             "收齐 10/21；赛后一周内报账"),
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
             "校验接线设计 → 落原理图 → 搭面包板 demo 交李沛阳（9/28）→ 边等冒烟反馈边布板 → 9/30 前下单（冒烟有问题则 10/3 前改板重下）→ 收货焊接 → 10/11 点亮并交接线图",
             "李沛阳的面包板 demo（9/28）；李沛阳接线图与点亮结果（10/11）",
             "李沛阳的《引脚分配表》v0.2；李昕程的关键尺寸与《车架与 PCB 对接约定》（9/24）；熊雅琦的元件与封装核对（9/28）；李沛阳的冒烟反馈（10/3 前）",
             "原理图+约定 9/24；面包板 demo 9/28；下单 9/30 前；点亮 10/11"),
        ],
    },
    {
        "name": "熊雅琦 —— A01 · S01 · S02",
        "rows": [
            ("A01", "循迹控制",
             "灰度处理 + PID，处理弯道与回环；本人上车调参，10/18 能跑弯、10/21 出全赛道稳定版",
             "毕烨恺的全赛道稳定循迹版（他做避障回线的基座）",
             "李沛阳的驱动接口初版（9/30，先离线写代码）；王政淋的面包板 demo（9/28）先验灰度读数；李昕程的实车 v1（10/13）",
             "9/30 起可编；10/14 首次上车；10/18 能跑弯；10/21 稳定版"),
            ("S01", "辅助电路",
             "打板前核对元件、封装与接线，避免打板出错",
             "王政淋一份核对过的落图依据",
             "王政淋的 `netlist.json` 与原理图草案",
             "9/28 前"),
            ("S02", "辅助装配",
             "装配打下手、拍照记录、固定走线",
             "李昕程的装配照片与记录",
             "李昕程的装配安排与到货材料",
             "10/8~10/13；定版 10/17~10/18"),
        ],
    },
    {
        "name": "李昕程 —— M01 · M02",
        "rows": [
            ("M01", "结构设计与建模",
             "整车模型、关键尺寸、关键孔位、加工图，导出件传 `mech/设计文件/`；与王政淋一起定《车架与 PCB 对接约定》",
             "王政淋的板形依据、M02 的加工图、全组的尺寸确认（红线 ≤250×200×200mm）",
             "王政淋的板子需求（对接讨论里定）和电控件尺寸",
             "设计文件 9/24；模型冻结 9/28"),
            ("M02", "加工与装配",
             "加工材料与紧固件自己买（9/24 前下单）；用中心设备加工并全程拍照录像；装出实车 v1，10/18 定版车架",
             "熊雅琦、毕烨恺的实车；毕烨恺的加工证据",
             "自己的加工图（9/24）；王政淋的点亮结果（10/11）用于装电控",
             "加工 10/8~10/11；实车 v1 10/13；定版 10/18"),
        ],
    },
    {
        "name": "毕烨恺 —— A02 · D02",
        "rows": [
            ("A02", "避障与回线",
             "超声波避障：单测 → 检测 demo → 绕行 + 回线 → 终点精停；本人上车调参",
             "全组的成品功能：4 障全过 + 精停",
             "李沛阳的遥测与驱动接口（9/30）；王政淋的面包板 demo（9/28）先验超声波读数；李昕程的实车 v1（10/13）；熊雅琦的稳定循迹版（10/21）",
             "单测 10/14；demo 10/18；绕行回线 10/21；达标 10/25"),
            ("D02", "文档与展示材料",
             "做 PPT、答辩问答、演示视频；收全员素材",
             "全组一套完整的展示材料",
             "全组素材（10/18 前）；李沛阳的成本台账（10/21）；李昕程的加工证据（加工期间）",
             "草稿 10/21；冻结 10/25"),
        ],
    },
]

# 共同任务（不是新增编号，属于 M01 与 H02 的交接点）
JOINT = {
    "name": "车架与 PCB 对接约定（王政淋 + 李昕程，熊雅琦辅助）",
    "do": "两人一起商量，把“板子做多大、装在底板哪里、打几个孔、线从哪边出来、上面留多高”写清楚，"
          "填进 `mech/车架与PCB对接约定.md`。只写对方必须知道的尺寸，细节两人自己定，不用写很长。",
    "give": "王政淋画 PCB 的尺寸依据、李昕程装车的同一份尺寸",
    "need": "熊雅琦帮忙核对元件位置",
    "ddl": "9/24 前定稿；之后要改，两人说一声即可（别单方面改）",
}

# ---------------------------------------------------------------------------
# 4. 交接时间线（按节点分组，不逐日罗列）
# ---------------------------------------------------------------------------
TIMELINE = [
    {
        "period": "9/21~9/27 · 原理图与面包板",
        "events": [
            "**9/24**：王政淋 ＋ 李昕程 定稿《车架与 PCB 对接约定》（熊雅琦辅助核对）",
            "**9/24**：王政淋 落定原理图；李沛阳《引脚分配表》校验完成",
            "**9/24**：李昕程 交设计文件（整车模型 ＋ 关键尺寸 ＋ 加工图）→ `mech/设计文件/`",
            "**9/24 前**：李昕程 自行下单加工材料与紧固件",
            "**9/25~9/27**：王政淋 按原理图搭面包板 demo（双 I2C、灰度、超声波、编码器、TB6612 逻辑、降压与 ADC 分压）",
            "面包板只验信号级；大电流与电源保护留到 PCB 验证",
        ],
    },
    {
        "period": "9/28~9/30 · 关键下单窗口",
        "events": [
            "**9/28**：王政淋 交面包板 demo → 李沛阳；李沛阳 开始桌面冒烟",
            "**9/28**：《引脚分配表》冻结；李昕程 模型冻结；熊雅琦 完成元件与封装核对",
            "**9/30**：王政淋 PCB 下单 ＋ 电子元件到货清点；李沛阳 交驱动接口初版",
        ],
    },
    {
        "period": "10/1~10/7 · 国庆假（硬件停）",
        "events": [
            "李沛阳：驱动逐个冒烟、P02 初版修订",
            "熊雅琦、毕烨恺：离线写算法代码",
            "**10/3 前**：冒烟发现原理图问题就改板重下；没问题按 9/30 下单推进",
        ],
    },
    {
        "period": "10/8~10/11 · 节后硬件攻坚",
        "events": [
            "**10/8~10/11**：李昕程 中心设备加工车架与底板（全程拍照录像）",
            "**10/11**：王政淋 交 PCB 点亮 ＋ 接线图 → 李沛阳",
        ],
    },
    {
        "period": "10/12~10/18 · 上车调试周",
        "events": [
            "**10/13**：李昕程 交实车 v1（能通电、能推着走）→ 熊雅琦、毕烨恺",
            "**10/14**：李沛阳 交驱动接口冻结版 ＋ 遥测上车；熊雅琦、毕烨恺开始上车调参",
            "**10/18**：全组 交素材 → 毕烨恺；李昕程 交定版车架",
        ],
    },
    {
        "period": "10/19~10/25 · 收口周",
        "events": [
            "**10/21**：熊雅琦 交稳定循迹版（全赛道）→ 毕烨恺；李沛阳 交成本台账 → 毕烨恺",
            "**10/25**：毕烨恺 交 4 障全过 ＋ 精停 ＋ 全套材料",
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
    ("9/24（周四）", "原理图定稿 ＋ 车架/PCB 对接约定；设计文件交付"),
    ("9/28（周一）", "面包板 demo 交付并开始冒烟；《引脚分配表》冻结；车架模型冻结"),
    ("9/30（周三）", "PCB 下单（节前窗口）＋ 驱动接口初版"),
    ("10/11（周日）", "PCB 点亮 ＋ 接线图"),
    ("10/13（周二）", "实车 v1 能通电"),
    ("10/14（周三）", "驱动接口冻结；第一次上车调试"),
    ("10/18（周日）", "循迹初版能跑弯、避障能发现障碍；定版车架"),
    ("10/21（周三）", "稳定循迹全赛道；成本台账；展示材料草稿"),
    ("10/25（周日）", "全功能冻结：4 障全过 ＋ 精停 ＋ 全套材料"),
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
    "掐好节前窗口：PCB 下单 9/30 前（王政淋）、加工材料下单 9/24 前（李昕程）；国庆期间快递与加工停摆。",
    "对接约定定稿后改东西要双方说一声，不能单方面改（《引脚分配表》《车架与 PCB 对接约定》）。",
    "算法谁写的谁上车调，调参不外包。",
    "每次上车测试留日志 + 一句话结论（用 `tools/run.py` 自动落盘）。",
    "判罚、评分类问题先查 `参考资料/原文件/` 原文，不确定就在群里问，别自己猜。",
]

# ---------------------------------------------------------------------------
# 8. 已完成（截至 9/20）
# ---------------------------------------------------------------------------
COMPLETED = [
    ("D01 仓库与协作规范",
     "仓库骨架、两个一键脚本、说明书、练习场、各工作目录 README 就绪；5 人全部入仓"),
    ("P01 阶段 1",
     "一键烧录 ＋ 100Hz 遥测 ＋ 自动存档出图 ＋ 在线调参命令（9/13 跑通）"),
    ("引脚与电气设计 v0.2",
     "`docs/引脚分配表.md`、`hardware/原理图草案/`（4 张图 ＋《电气连接规范》＋ 机器可读 `netlist.json`）"),
    ("H01 选型与采购",
     "BOM 与器件资料索引定稿；万向轮、编码器座、HC-SR04、蓝牙、电源座等已下单；"
     "电机确认带编码器；避障定案 HC-SR04 ×2"),
    ("M01 建模起步",
     "三轮构型整车模型、车架与底板截图、实物照片"),
    ("资料与协作",
     "参考资料补齐（器件手册、实物照片）；计划全面日期化；`mech/设计文件/`、`加工证据/` 与对接约定模板就位"),
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
    L.append(f"# {TITLE}（v2.3）")
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
    L.append("## 8. 已完成（截至 9/20）")
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

    A(r"\sect{8. 已完成（截至 9/20）}")
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
