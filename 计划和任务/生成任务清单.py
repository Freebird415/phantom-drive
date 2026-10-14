# -*- coding: utf-8 -*-
"""生成《任务清单》的 Markdown 与 PDF（同一份数据）。

唯一事实来源：本文件下面的数据表。改完重跑：

    python 生成任务清单.py              # 生成 任务清单.md（唯一分发件）
    python 生成任务清单.py              # 只生成 任务清单.md / 任务清单.tex

输出（同目录）：
    任务清单.md     与 PDF 同内容的纯文本版（方便贴群、diff、被 agent 读）
    任务清单.tex    LaTeX 源（由本脚本生成，勿手改）
    任务清单.pdf    已停用（2026-10-12 删除；保留 --build 能力，按需生成）

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
META = "5 人 · 13 项任务 · v2.10 · 更新：2026-10-12"
LEAD = ("本清单只讲两件事：**已经完成了什么**、**接下来谁做什么**。"
        "与 `项目记录/开发记录.md` 同步；如两份说法不一致，以《开发记录》为准。所有截止时间统一用日期表示。")

# ---------------------------------------------------------------------------
# 0. 关键时间与红线
# ---------------------------------------------------------------------------
KEY_TIME = [
    ("现在", "10/12（周一）—— **比赛日期已确定：10/21（周三）开始**（第 8 周周三；比原预测 10/26~11/1 提前 5 天）"),
    ("课满，不排任务", "**10/12（周一）~10/13（周二）**——课多，连续两天不安排任务；重心放在 **10/16（周五）、10/17（周六）**"),
    ("全部任务死线", "**10/18（周日）24:00** —— 此后不再动功能，只做彩排与备份"),
    ("剩余差距", "只差两件：**A02 避障闭环**（陀螺零反馈 + 回线）＋ **避障 × 循迹 × 精停 融合**"),
    ("缓冲", "10/19~10/20 不排新功能：彩排 / 备份 / 备件 / 现场标定预演"),
    ("比赛", "10/21（周三）起 —— 比赛 ＋ 演讲"),
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
        "name": "李沛阳（组长）—— D01 · P01 · P02 · F01",
        "rows": [
            ("D01", "项目仓库与协作规范",
             "✅ **已完成**：仓库骨架、四个一键脚本（拉取/提交/工具箱/蓝牙日志）、说明书、各目录 README；结构 v2 + AI 留言板（D-08）",
             "全组：仓库目录、提交规范、AI 入口",
             "",
             "✅ 已完成"),
            ("P01", "反馈链路与工具链",
             "阶段 1 ✅ / 阶段 2 ✅（板载快照 ＋ 无线日志）；剩阶段 3：参数掉电保存、绝对计数锚帧（定死 mm/计数）、电压采样",
             "熊雅琦、毕烨恺跑车后的数据分析工具链",
             "10/15 熊雅琦的终点线现场实测数据（几路黑）；15° 标测结果",
             "阶段 3 收尾 **10/16**"),
            ("P02", "底层驱动与框架",
             "✅ **已完成**：8 驱动实机验收（除已放弃的 OLED）；接口说明 9/30 交、10/10 起在用",
             "熊雅琦、毕烨恺的驱动接口说明（`firmware/文档/驱动接口说明.md`）",
             "《引脚分配表》v3.0（已冻结）；固件 Flash 余量仅 7.9% ⇒ 改代码前先算体积",
             "✅ 已达成（10/10 起在用）；之后只加功能"),
            ("F01", "账务与报销",
             "收集发票与商品照片、填成本台账、赛后统一报账",
             "毕烨恺的成本台账（进 PPT）",
             "全组的电子发票 ＋ 收货照片 ＋ 一句用途说明",
             "收齐 **10/18**；赛后一周内报账"),
        ],
    },
    {
        "name": "王政淋 —— H01 · H02",
        "rows": [
            ("H01", "选型与采购",
             "✅ 主体完成（BOM/下单/到货清点）；剩：选型决策表成文、备件清点",
             "全组齐套可用的电子元件与备件",
             "",
             "选型决策表 **10/17**；备件清点 **10/18**"),
            ("H02", "电路与电气集成",
             "✅ 主体完成（原理图落定、PCB v2.1 装车、电器验收正常）；剩：点亮记录与接线图入库、12V/5V/3V3 实测留档、备件与工具盘点",
             "全组：接线图台账（查线用）、备件清单",
             "PCB v2.1 已装车（10/10）；`完整接线.md` 为接线唯一记录",
             "**10/14 前**入库；**10/18** 备件盘点"),
        ],
    },
    {
        "name": "熊雅琦 —— A01 · S01 · S02",
        "rows": [
            ("A01", "循迹控制",
             "✅ **主体完成**（10/10 无障碍全程通过、精停已实现、v8 基线冻结）；剩：陪 A02 做融合（接管暂停后交还的第一拍平滑性、绕障中看到线立刻结束机动的验证）",
             "毕烨恺的全赛道稳定循迹版（他做避障回线的基座）",
             "李沛阳的驱动接口与板载快照；李昕程的实车",
             "融合 **10/17~10/18**"),
            ("S01", "辅助电路",
             "打板前核对元件、封装与接线，避免打板出错",
             "王政淋一份核对过的落图依据",
             "王政淋的 `netlist.json` 与电气设计",
             "✅ 已完成（随电气设计 9/23 冻结）"),
            ("S02", "辅助装配",
             "装配打下手、拍照记录、固定走线",
             "李昕程的装配照片与记录",
             "李昕程的装配安排与到货材料",
             "✅ 已完成（随 M02；照片入 `mech/加工证据/`）"),
        ],
    },
    {
        "name": "李昕程 —— M01 · M02",
        "rows": [
            ("M01", "结构设计与建模",
             "✅ **已完成**：整车模型、截图、整车 STEP ＋ 底板 STL/DXF ＋ 建模源文件（.SLDPRT）已入库；尺寸与孔位已与王政淋当面确认（符合红线 ≤250×200×200），无需成文",
             "全组：设计文件、板形与孔位依据",
             "",
             "✅ 已完成"),
            ("M02", "加工与装配",
             "✅ **已完成**：激光切割 / 3D 打印 / 装配证据已入库；**定版车架 = `mech/设计文件/` 最新版（已成车、在用）**",
             "熊雅琦、毕烨恺的实车；毕烨恺的加工证据",
             "",
             "✅ 已完成"),
        ],
    },
    {
        "name": "毕烨恺 —— A02 · D02",
        "rows": [
            ("A02", "避障与回线",
             "按《A02 避障算法迭代建议》四步：①脱离→回归（不依赖陀螺）②固定障碍矩阵 ③算一次执行到底 ④与 A01/精停融合",
             "全组的成品功能：4 障全过 ＋ 精停",
             "李沛阳的遥测与驱动接口（已冻结）；15° 实测首次检出距离；熊雅琦的稳定循迹版",
             "标测＋第1步 **10/14**；第2步 **10/15**；第3步 **10/16**；融合 **10/16~17**；多轮 **10/17**；达标 **10/18**（关卡：10/16 / 10/17）"),
            ("D02", "文档与展示材料",
             "PPT、答辩问答、演示视频；收全员素材",
             "全组一套完整的展示材料",
             "全组素材；李沛阳的成本台账；李昕程的加工证据",
             "素材 **10/17**；草稿 ＋ 冻结 **10/18**"),
        ],
    },
]

# 共同任务（不是新增编号，属于 M01 与 H02 的交接点）
JOINT = {
    "name": "车架与 PCB 的尺寸与孔位（王政淋 + 李昕程，熊雅琦辅助核对）",
    "do": "板子做多大、装在底板哪里、打几个孔、线从哪边出来、上面留多高——**两人当面商定即可，不用写文档**。",
    "give": "王政淋画 PCB 的尺寸依据、李昕程装车的同一份尺寸",
    "need": "熊雅琦帮忙核对元件位置",
    "ddl": "✅ 9/24 已当面商定（PCB 已按此打板到货）；后续要改，两人说一声",
}

# ---------------------------------------------------------------------------
# 4. 交接时间线（按节点分组，不逐日罗列）
# ---------------------------------------------------------------------------
TIMELINE = [
    {
        "period": "10/12~10/13 · 课满，不排任务",
        "events": [
            "这两天课多，**连续两天不安排任务**；有空只看一眼留言板与决策登记，不动代码。",
        ],
    },
    {
        "period": "10/14（周三 · 工创课，全组在场）· 集中日：标测 ＋ 第 1 步",
        "events": [
            "毕烨恺 ＋ 李沛阳：**15° 探头几何标测**（5 分钟）＋ 给 `gz` 加失效打点；**避障改架构：不依赖陀螺**",
            "毕烨恺：**A02 第 1 步**（脱离→回归）：车载触发，左右各 **10/10**、回归后 1 秒内 `|err| ≤ 1`、全程不反转；记 `R_min`／外移里程／回归误差",
            "王政淋：H02 点亮记录 ＋ 接线图入库",
        ],
    },
    {
        "period": "10/15（周四）· 第 2 步：固定障碍矩阵",
        "events": [
            "毕烨恺：贴线边／半压线／偏 35° × 左右 × 2 速度，每格 **5/5、零接触**；触发距离与让开量记成表",
        ],
    },
    {
        "period": "10/16（周五 · 重心①）· 第 3 步 ＋ 融合首轮 —— 【关卡①】",
        "events": [
            "毕烨恺：用第 2 步数据拟合 `d_trigger = a + b·v`，把写死的量换成算出来的量；**覆盖没测过的组合**",
            "全组：**全赛道首次跑通**（4 障 ＋ 精停）；A02 交还 A01 的平滑性检查",
            "**关卡①（中午前定）**：第 3 步不过 → 当天下午起退回第 2 步固定轨迹版，用它做融合",
        ],
    },
    {
        "period": "10/17（周六 · 重心②）· 多轮验收 ＋ 定版 ＋ 素材 —— 【关卡②】",
        "events": [
            "全组：全赛道多轮（目标 ≥5 轮、≥3 轮全过）",
            "全组：**素材交齐** → 毕烨恺（D02 草稿）；李沛阳：成本台账",
            "**关卡②（中午前定）**：全流程仍不通 → 当天下午起切 v9 保守版，主保 P0",
        ],
    },
    {
        "period": "10/18（周日）· 终极验收 ＋ 冻结 ＋ 演练 —— 24:00 死线",
        "events": [
            "上午：全赛道验收（**连续 2 轮 4 障全过 ＋ 精停**）",
            "下午：材料冻结（PPT／文档／视频／台账）；**比赛日演练**（按 `比赛日操作卡` 全流程走一遍）",
            "晚上：备份（固件 bin ＋ 算法快照 ＋ git 标签）＋ 备件清点",
            "**24:00 全部任务关闭**",
        ],
    },
    {
        "period": "10/19~10/20 · 缓冲（不排新功能）",
        "events": [
            "彩排／备份复核／备件／现场标定预演（灰度、终点线、`STOPDIST`）",
        ],
    },
    {
        "period": "10/21（周三）· 比赛开始",
        "events": [
            "按 `项目记录/比赛日操作卡.md` 执行（发车／终点／合规红线）",
        ],
    },
]

# ---------------------------------------------------------------------------
# 5. 里程碑
# ---------------------------------------------------------------------------
MILESTONES = [
    ("10/12~10/13", "课满，不排任务"),
    ("10/14（周三）", "15° 标测 ＋ 陀螺失效打点；A02 第 1 步 10/10；H02 点亮记录＋接线图入库"),
    ("10/15（周四）", "A02 第 2 步：障碍矩阵 5/5 零接触"),
    ("10/16（周五）", "A02 第 3 步 ＋ 全赛道首通（4 障 ＋ 精停）【关卡①】"),
    ("10/17（周六）", "多轮验收（≥3 轮全过）；素材交齐【关卡②】"),
    ("10/18（周日）", "**24:00 死线**：验收（连续 2 轮 4 障全过 ＋ 精停）＋ 材料冻结 ＋ 比赛日演练 ＋ 备份"),
    ("10/19~10/20", "缓冲：彩排 / 备份 / 备件 / 现场标定预演（不排新功能）"),
    ("10/21（周三）", "比赛开始"),
]

# ---------------------------------------------------------------------------
# 6. 目标分级与止损（高容错）
# ---------------------------------------------------------------------------
GOALS = [
    ("P0 保底（必须拿到）", "4 赛段 ＋ 终点精停（车尾不出线）＋ 不脱线不逆行",
     "不依赖避障；即使 A02 全部失败也要保证"),
    ("P1 目标", "4 障全过（+40 分）",
     "按《A02 避障算法迭代建议》四步走：第1步 10/14 → 第2步 10/15 → 第3步 10/16 → 融合 10/16~17 → 验收 10/18"),
    ("P2 不做", "提速（时间分）",
     "冻结期不动 A01 基线（E-35）；提速只在不碰接口时单独评估"),
]

STOP_LOSS = [
    "**10/16 中午 · 关卡①**：A02 第 3 步（瞬时计算）没过 → 当天下午起退回**第 2 步固定轨迹版**（已 5/5 验收的参数化版本），用它做融合，不再追新方案",
    "**10/17 中午 · 关卡②**：全流程（4 障 ＋ 精停）没跑通 → 当天下午起切**保守版**：回退已知能绕第一障的 v9 版本，主保 P0",
    "**任何时刻**：改动只在自己文件（A01→`app_trace.c`、A02→`app_avoid.c`）；每版存算法快照 ＋ 实测日志，随时可回退",
    "**现场兜底**：无线日志断了走板载快照（`DUMP`）；灰度现场按需再校（已随循迹调通）；陀螺不可用不影响避障（架构不依赖它）",
]

# ---------------------------------------------------------------------------
# 7. 协作纪律
# ---------------------------------------------------------------------------
DISCIPLINE = [
    "倒排冲刺：只做能提升 P0/P1 的事；**10/19 起不再动任何功能**。",
    "每步都要验收数字（10/10、5/5、零接触、`|err| ≤ 1`）——“跑通了”不算验收。",
    "只改自己文件（A01→`app_trace.c`、A02→`app_avoid.c`）；每版存算法快照 ＋ 实测日志，随时可回退；**不动 A01 基线**（E-35）。",
    "不依赖单点：陀螺不进避障计算；无线断了走板载快照；灰度已随循迹调通（无需重校，现场按需再校）。",
    "跑一趟留一条：双击「蓝牙日志.bat」导出片段（自动带 `note.md` 摘要）；给别人看的结论写 `logs/共享_*.md`。",
    "提交：拉取 → 放入 → 提交；`commit`/`push` 要组长确认。",
    "判罚、评分类问题先查 `参考资料/原文件/` 原文；不确定的结论写成“推定/待验证”（15° 覆盖、mm/计数仍未标定）。",
]

# ---------------------------------------------------------------------------
# 8. 已完成（截至 10/12）
# ---------------------------------------------------------------------------
COMPLETED = [
    ("D01 仓库与协作规范",
     "仓库骨架、一键脚本（拉取/提交/工具箱/蓝牙日志）、说明书、各工作目录 README 就绪；**结构 v2 + AI 留言板**（2026-10-10）；5 人全部入仓"),
    ("P01 阶段 1",
     "一键烧录 ＋ 100Hz 遥测 ＋ 自动存档出图 ＋ 在线调参命令（9/13 跑通）"),
    ("引脚与电气设计 v3.0",
     "`docs/引脚分配表.md`（**9/23 提前冻结**）、`hardware/电气设计/`（4 张图 ＋《电气连接规范》＋ `完整接线.md` ＋ `netlist.json` ＋ 可执行 `自检.py`，106 项全过）"),
    ("P02 驱动与接口",
     "8 个器件驱动全部实机验收（除已放弃的 OLED）；`firmware/文档/驱动接口说明.md` 已交并在用；算法层代码缝（A01 `app_trace.c` / A02 `app_avoid.c`，默认只算不发）"),
    ("P01 阶段 2 · 板载快照",
     "`$T` 接真实灰度；**板载快照全链路**（RAM 记录 → 停车写 Flash → 掉电不丢 → `tools/snapshot.py` 读出图）；`track.py` 重建实际轨迹"),
    ("P01 · 无线日志工具链（网页界面）",
     "双击「蓝牙日志.bat」→ 浏览器界面实时看帧；**监测模式**（只显示不写文件 + 空格一键记录 + 最近 ~65s 回溯入库）；拖入 CSV 导入；按键导出片段（自动带 `derived.csv` + `note.md` 摘要）；超声波两列 + 存在性保证"),
    ("H01 选型与采购",
     "BOM 与器件资料索引定稿；万向轮、编码器座、HC-SR04、电源座等已下单；电机确认带编码器；避障定案 HC-SR04 ×2；**PCB v2.1 到货并装车（10/10）**"),
    ("H02 电气集成",
     "原理图落定、下单到位；**PCB v2.1 已装车、电器功能验收正常（10/10）**（MPU6050 / 灰度 / 超声波 ×2 / 按键 / 蜂鸣器 / TB6612 逻辑；OLED 已放弃）；固件 33 脚与 v3.0 逐脚一致；**电气文档已按实物原理图逐针校正（10/10 深夜，E-34）**"),
    ("A01 循迹（含算法迭代）",
     "**10/10「全程1」无障碍全程试跑通过**（2126 帧：循迹 2077 / 丢线找线 1 / 精停 48；过弯平稳，用户确认循迹部分完成）；**算法 v8 + 保守基线**（默认/上限 22%、不命令反转）；待办：带障碍全赛道 + 提速 ⇒ 见 `项目记录/版本索引.md`"),
    ("A02 避障与回线",
     "**v1~v4 已上车试跑**（20cm 双次确认 + 3cm 刹停 + 100Hz 仲裁；v9 已归档、标签 `a02-v1-20261010-20cm-trial`）；**当前卡点：陀螺零反馈（gz 恒 0）⇒ 持续打舵、约半圈后外廓保护（v4 未修复）**；20cm 试跑第一障碍绕开、第二障碍弯道回线失败；15° 新几何仍未标测"),
    ("M01/M02 结构设计与加工",
     "三轮构型整车模型与截图、整车 STEP ＋ 底板 STL/DXF ＋ 建模源文件（.SLDPRT）已入库；**车架与 PCB 的尺寸/孔位已与王政淋当面确认，符合红线（≤250×200×200），无需成文**；激光切割/3D 打印/装配证据已入库；**成车已装到能跑（已在用最新版 PCB v2.1）** —— 机械侧无待办"),
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
    L.append(f"# {TITLE}（v2.10）")
    L.append("")
    L.append(f"> {SUBTITLE}")
    L.append(f"> {META}")
    L.append(">")
    L.append(f"> {LEAD}")
    L.append(">")
    L.append("> 本文件由 `生成任务清单.py` 生成（**唯一分发件**；PDF 已停用 2026-10-12）。")
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
    L.append("每行读法：**做**什么 → **交**给谁什么 → **需**要谁的什么 → 什么时候要。**已完成的任务标 ✅，不删。**")
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
    L.append("只列重要节点；按天与关卡分断。")
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
    L.append("## 6. 目标分级与止损（高容错）")
    L.append("")
    L.append("| 级别 | 拿到什么 | 说明 |")
    L.append("|---|---|---|")
    for a, b, c in GOALS:
        L.append(f"| {a} | {b} | {c} |")
    L.append("")
    L.append("**止损点（到点不达标就走降级路，不硬撑）**")
    L.append("")
    for s in STOP_LOSS:
        L.append(f"- {s}")
    L.append("")
    L.append("## 7. 协作纪律")
    L.append("")
    for i, d in enumerate(DISCIPLINE, 1):
        L.append(f"{i}. {d}")
    L.append("")
    L.append("## 8. 已完成（截至 10/12）")
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
    A(r"{\small 只列重要节点；按天与关卡分断。\par}")
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

    A(r"\sect{6. 目标分级与止损（高容错）}")
    A(r"{\small")
    A(r"\begin{tabular}{@{}L{3.6cm}L{6.4cm}L{8.8cm}@{}}")
    A(r"\toprule")
    A(r"\textbf{级别} & \textbf{拿到什么} & \textbf{说明}\tabularnewline")
    A(r"\midrule")
    for a, b, c in GOALS:
        A(tex_escape(a) + r" & " + tex_escape(b) + r" & " + tex_escape(c) + r"\tabularnewline")
    A(r"\bottomrule")
    A(r"\end{tabular}")
    A("")
    A(r"}")
    A("")
    A(r"{\small \textbf{止损点（到点不达标就走降级路，不硬撑）}")
    A("")
    A(r"\begin{itemize}\setlength{\itemsep}{2pt}")
    for s in STOP_LOSS:
        A(r"\item " + tex_escape(s))
    A(r"\end{itemize}")
    A("")
    A(r"}")
    A("")

    A(r"\sect{7. 协作纪律}")
    A(r"\begin{enumerate}\setlength{\itemsep}{2pt}")
    for d in DISCIPLINE:
        A(r"\item " + tex_escape(d))
    A(r"\end{enumerate}")
    A("")

    A(r"\sect{8. 已完成（截至 10/12）}")
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
