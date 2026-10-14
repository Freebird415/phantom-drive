# -*- coding: utf-8 -*-
"""生成《15° 探头几何标测 · 记录表》Excel（给毕烨恺现场手抄用）。

用法：  python 生成探头标测表.py        # 覆盖生成 探头几何标测_记录表.xlsx

设计原则：**每个点位都对着 A02 的一个具体参数**，不为测而测。
    第 1、2 组 → 首检距离（右）  ⇒ 触发门限 d_trigger 的常数项
    第 3 组    → 首检距离（左）  ⇒ 与右侧镜像校验 + 左侧门限
    第 4 组    → 横向覆盖/判侧   ⇒ 贴线边 60mm 障碍可见吗？中央有盲区吗？谁先看到？
    第 5 组    → 远限 / 近限     ⇒ 触发上限、极近兜底阈值
    第 6 组    → 大障碍对比      ⇒ 最坏情况（窄障碍）的安全裕度
共 **26 个点位**（约 8~10 分钟），留 4 点余量给现场补测。
"""
from __future__ import annotations

from pathlib import Path

from openpyxl import Workbook
from openpyxl.styles import Alignment, Border, Font, PatternFill, Side
from openpyxl.utils import get_column_letter
from openpyxl.worksheet.datavalidation import DataValidation

HERE = Path(__file__).resolve().parent
OUT = HERE / "探头几何标测_记录表.xlsx"

# ---------------------------------------------------------------- 样式
F_TITLE = Font(name="微软雅黑", size=13, bold=True, color="FFFFFF")
F_HEAD = Font(name="微软雅黑", size=10, bold=True, color="FFFFFF")
F_GROUP = Font(name="微软雅黑", size=10, bold=True, color="1F3864")
F_BODY = Font(name="微软雅黑", size=10)
F_NOTE = Font(name="微软雅黑", size=10, color="7F7F7F")

FILL_TITLE = PatternFill("solid", fgColor="1F3864")
FILL_HEAD = PatternFill("solid", fgColor="2E5496")
FILL_GROUP = PatternFill("solid", fgColor="DEEAF6")
FILL_FILLIN = PatternFill("solid", fgColor="FFF2CC")   # 待填 = 黄
FILL_KEY = PatternFill("solid", fgColor="FCE4D6")      # 关键行 = 橙

THIN = Side(style="thin", color="AAAAAA")
BOX = Border(left=THIN, right=THIN, top=THIN, bottom=THIN)
CENTER = Alignment(horizontal="center", vertical="center", wrap_text=True)
LEFT = Alignment(horizontal="left", vertical="center", wrap_text=True)

NOTE_ALLOWED = "稳定,跳动,无(-1),无(-2),太近(-5),限流(-6),其他"

SMALL = "小障碍 60×60×150（阿凡提）"
BIG = "大障碍 150×100×150（莫高窟）"
EDGE = "内边贴线（离车中轴 10）"
CENTERLINE = "中心对中轴"

XS_COARSE = [1000, 800, 600, 450, 300]          # 第 1/3 组粗扫
XS_BIG = [800, 400]                             # 第 6 组
YS_SIDE = [0, 60, 120, 200, -60, -120]          # 第 4 组（横向覆盖/判侧）

ROWS: list[tuple[str, list[tuple]]] = [
    ("第 1 组 ⭐ 右侧 · 小障碍 · 贴线边 · 粗扫 —— 测『首检距离（右）』（A02 触发门限的常数项）",
     [(SMALL, "右侧", x, EDGE, 10, x in (600, 450, 300)) for x in XS_COARSE]),

    ("第 2 组 ⭐ 右侧 · 细扫 —— 把『首检距离（右）』精度做到 ±25mm"
     "（做完第 1 组回来填 X：抄第 1 组首次出数字附近，每档 50mm）",
     [(SMALL, "右侧", None, EDGE, 10, True) for _ in range(5)]),

    ("第 3 组 ⭐ 左侧 · 小障碍 · 贴线边 · 粗扫 —— 测『首检距离（左）』，同时充当右侧的镜像校验",
     [(SMALL, "左侧", x, EDGE, -10, x in (600, 450, 300)) for x in XS_COARSE]),

    ("第 4 组 ⭐ 横向覆盖 / 判侧（X 用第 1 组测出的『首检距离』，没测出来就填 500）"
     " —— 回答：贴线边能看见吗？中央有盲区吗？左右哪颗先看到？",
     [(SMALL, "中央" if y == 0 else ("右侧" if y > 0 else "左侧"), None, CENTERLINE, y, y == 0)
      for y in YS_SIDE]),

    ("第 5 组 ⭐ 界限值 —— 最远还能测到（触发上限）与最近还能测到（极近兜底）",
     [(SMALL, "右侧", 1200, EDGE, 10, True),
      (SMALL, "右侧", 150, EDGE, 10, True),
      (SMALL, "右侧", 100, EDGE, 10, True)]),

    ("第 6 组 大障碍 · 贴线边 · 右侧 —— 与第 1 组同 X 对比：窄障碍（60mm）是最坏情况，留多少裕度",
     [(BIG, "右侧", x, EDGE, 10, True) for x in XS_BIG]),
]


# ---------------------------------------------------------------- 小工具
def _title(ws, text, ncol, row=1, height=26):
    ws.merge_cells(start_row=row, start_column=1, end_row=row, end_column=ncol)
    c = ws.cell(row=row, column=1, value=text)
    c.font, c.fill, c.alignment = F_TITLE, FILL_TITLE, CENTER
    ws.row_dimensions[row].height = height


def _head(ws, row, labels, widths=None):
    for i, lab in enumerate(labels, start=1):
        c = ws.cell(row=row, column=i, value=lab)
        c.font, c.fill, c.alignment, c.border = F_HEAD, FILL_HEAD, CENTER, BOX
    ws.row_dimensions[row].height = 34
    if widths:
        for i, w in enumerate(widths, start=1):
            ws.column_dimensions[get_column_letter(i)].width = w


def _group(ws, row, text, ncol):
    ws.merge_cells(start_row=row, start_column=1, end_row=row, end_column=ncol)
    c = ws.cell(row=row, column=1, value=text)
    c.font, c.fill, c.alignment = F_GROUP, FILL_GROUP, LEFT
    for i in range(1, ncol + 1):
        ws.cell(row=row, column=i).border = BOX
    ws.row_dimensions[row].height = 20


# ---------------------------------------------------------------- sheet：说明
def build_help(wb):
    ws = wb.create_sheet("说明")
    ws.column_dimensions["A"].width = 4
    ws.column_dimensions["B"].width = 114
    _title(ws, "15° 探头几何标测 · 现场说明（毕烨恺 · 一人完成 · 26 个点位 ≈ 10 分钟）", 2)

    def sec(text):
        r = ws.max_row + 2
        ws.merge_cells(start_row=r, start_column=1, end_row=r, end_column=2)
        c = ws.cell(row=r, column=1, value=text)
        c.font, c.fill, c.alignment = F_GROUP, FILL_GROUP, LEFT
        return r

    def bullet(text, font=None):
        r = ws.max_row + 1
        ws.cell(row=r, column=1, value="·").alignment = CENTER
        c = ws.cell(row=r, column=2, value=text)
        c.font, c.alignment = font or F_BODY, LEFT
        ws.row_dimensions[r].height = max(16, 15 * (1 + len(text) // 52))
        return r

    sec("0. 这是在量什么（每个点位都对着避障的一个参数，不是为测而测）")
    bullet("① 首检距离（左/右）：障碍物从多远开始被稳定看到 ⇒ A02 的触发门限（什么时候该减速/打舵）。")
    bullet("② 横向覆盖 / 判侧：贴线边的 60mm 障碍能不能看到、车正前方有没有盲区、左右哪颗先看到"
           " ⇒ 判侧规则与最坏情况的安全性。")
    bullet("③ 远限 / 近限：最远多少还能测、多近就测不到 ⇒ 触发上限与极近兜底阈值。")
    bullet("④ 大障碍对比 + 探头偏移：窄障碍是难看到的那一种；偏移把『探头面距离』换算成『车头到障碍』。")

    sec("1. 准备")
    bullet("① 车静置在直线段上（桌面或赛道直线都行），车头朝前，全程别碰车；上电，3V3 ≈ 3.3V。")
    bullet("② 电脑双击「蓝牙日志.bat」→ 等状态栏显示「蓝牙已连接」+ 有帧率（本轮不点开始接收、不用监测）。")
    bullet("③ 卷尺沿车头方向铺开，0 刻度对齐『车最前端』，铺好之后别再碰。")
    bullet("④ 障碍物：60×60×150（阿凡提）与 150×100×150（莫高窟）；没实物就用立起来的厚书（实心、够高）。")
    bullet("⑤ 笔记本放在能看见、又不挡探头的位置：摆好障碍物后人是回到电脑前读数的（障碍物不动，读数不会跑）。")
    bullet("⑥ 左右定义：沿车前进方向看，左手边 = 左探头那侧。")

    sec("2. 每个点位的动作（4 步，26 行都一样）")
    bullet("① 按表格的 X（从车最前端量）摆好障碍物 → ② 等 2 秒 → ③ 读一次，记『左①/右①』→ "
           "④ 再等 2 秒 → 读一次，记『左②/右②』。")
    bullet("⚠️ 面板走蓝牙（约每秒 2~4 次刷新）⇒ 不等到 2 秒就会读到上一个位置的值。"
           "两个数不一样时把差值写进备注，以 ② 为准。")
    bullet("⚠️ 没有数字就照实写：-1 / -2（没测到，正常）· -5（太近或太远）· -6（被限流跳过，正常）。"
           "不要写 0，也不要留空。")
    bullet("⚠️ 组内从远往近摆（表格已排好）：万一某次没刷新，数值会偏大偏远，一眼能看出来。")

    sec("3. 两组要按顺序做（后面两组依赖前面的结果）")
    bullet("第 2 组（右侧细扫）：做完第 1 组后，看『第一次出现数字』大概在哪个 X，"
           "把它上下各 100mm、每 50mm 一档，填进第 2 组的 X 列（共 5 档）。")
    bullet("第 4 组（横向覆盖）：X 用第 1 组测出的首检距离；填进 X 列后，只改横向 Y（0 / ±60 / ±120 / +200）。"
           "若第 1 组没测出首检，X 就填 500 并在备注写『X=500』。")
    bullet("第 1 组若在 X=1000 就已经有数字（说明很远就能看到）：备注写『补』，"
           "在表末尾补测 X=1200、1400 两档。")

    sec("4. 怎么算『看见了』")
    bullet("右侧数值出现有效距离（正数、且与卷尺刻度大致对得上）= 右探头看见它了；左侧同理。")
    bullet("同一行左右都有数字 ⇒ 两颗都看见了（通常先出现靠障碍物那一侧的那颗，这正是判侧要用的信息）。")

    sec("5. 出错 / 漏了怎么办（26 个点位就是按容错设计的）")
    bullet("抄错一个数：回到那个位置重读两遍，覆盖原格。")
    bullet("某一组没做或做废：跳过，备注写『废』——右侧有三层证据（粗扫 + 细扫 + 左侧镜像 + 大障碍），"
           "少一组仍能算出首检距离。")
    bullet("时间不够：只做 ⭐ 的第 1、3、5 组 ＋ 第 2 组即可（首检距离/判侧/界限都在里面）；第 4、6 组看情况。")

    sec("6. 做完怎么给组长")
    bullet("把这个 xlsx 直接发微信给组长（不用 git、不用开日志、不用拍照）。")

    sec("7. 表格怎么读")
    bullet("「记录表」：黄色格子 = 要你填；每组有一行说明（组内顺序别跳）。")
    bullet("「手工量」：卷尺量 3 个数，顺手填了（不填也能从记录表反算）。")
    bullet("「结论」：不用管，组长/AI 填（那里写着每个数会怎么用到 A02 上）。")
    return ws


# ---------------------------------------------------------------- sheet：记录表
def build_table(wb):
    ws = wb.create_sheet("记录表")
    head = ["序", "障碍物", "侧", "位置 X\n(mm，从车最前端)", "横向 Y\n(内边/中心，mm)",
            "左 读数①\n(mm)", "右 读数①\n(mm)", "左 读数②\n(mm)", "右 读数②\n(mm)",
            "备注（跳动 / 无(-1) / 无(-2) / 废）"]
    widths = [5, 26, 7, 13, 16, 11, 11, 11, 11, 34]
    _title(ws, "15° 探头几何标测 · 记录表（黄色格子要填；每行读两次，中间等 2 秒；共 26 行）", len(head))
    _head(ws, 2, head, widths)
    ws.freeze_panes = "A3"

    r, n = 3, 0
    for gtext, items in ROWS:
        _group(ws, r, gtext, len(head))
        r += 1
        for (obst, side, x, ykind, y, key) in items:
            n += 1
            if y is None:
                ytxt = ""
            elif ykind == EDGE:
                ytxt = f"{y}（内边）"
            elif ykind == CENTERLINE:
                ytxt = f"{y}（中心）"
            else:
                ytxt = ykind
            vals = [n, obst, side, x if x is not None else "自填", ytxt,
                    None, None, None, None, None]
            for i, v in enumerate(vals, start=1):
                c = ws.cell(row=r, column=i, value=v)
                c.font, c.border = F_BODY, BOX
                c.alignment = LEFT if i in (2, 10) else CENTER
                if i in (6, 7, 8, 9, 10):
                    c.fill = FILL_FILLIN
                elif key:
                    c.fill = FILL_KEY
            r += 1

    dv = DataValidation(type="list", formula1=f'"{NOTE_ALLOWED}"', allow_blank=True,
                        showErrorMessage=False, errorStyle="warning")
    dv.promptTitle = "备注"
    dv.prompt = "稳定 / 跳动 / 无(-1) / 无(-2) / 太近(-5) / 限流(-6) / 其他"
    ws.add_data_validation(dv)
    dv.add(f"J3:J{r - 1}")
    return ws


# ---------------------------------------------------------------- sheet：手工量
def build_manual(wb):
    ws = wb.create_sheet("手工量")
    head = ["项", "数值", "单位", "怎么量 / 用来干什么"]
    _title(ws, "卷尺/目测的 3 个数（顺手填；探头偏移也能从记录表反算）", len(head))
    _head(ws, 2, head, [30, 12, 8, 84])
    rows = [
        ("探头前端 → 车最前端", None, "mm",
         "卷尺从车最前端量到超声探头前表面（两侧各量一次取平均）⇒ 把探头读数换算成『车头到障碍』"),
        ("探头离地高度", None, "mm",
         "车放地面/桌面上，量探头中心离地高度 ⇒ 判断障碍物要多高才挡得住（规则里的障碍 150mm 高）"),
        ("循迹线宽", None, "mm", "量赛道黑线宽度（确认 20mm）⇒ 用于『贴线边』的横向换算"),
    ]
    for i, (a, b, c, d) in enumerate(rows, start=3):
        for j, v in enumerate((a, b, c, d), start=1):
            cell = ws.cell(row=i, column=j, value=v)
            cell.font, cell.border = F_BODY, BOX
            cell.alignment = LEFT if j in (1, 4) else CENTER
            if j == 2:
                cell.fill = FILL_FILLIN
    return ws


# ---------------------------------------------------------------- sheet：结论
def build_result(wb):
    ws = wb.create_sheet("结论")
    head = ["目标（A02 用在哪）", "结果", "单位", "置信度", "依据（记录表行号）", "备注"]
    _title(ws, "目标值 —— 现场不用填，组长/AI 算出来写这里（每行都写明它怎么用到 A02）", len(head))
    _head(ws, 2, head, [34, 12, 8, 10, 22, 62])
    rows = [
        ("首检距离（右）→ 触发门限 d_trigger 常数项", None, "mm", "", "",
         "贴线边小障碍第一次出现有效读数的 X（第 1、2 组，含 ±25mm 细扫）"),
        ("首检距离（左）→ 判侧/左侧门限", None, "mm", "", "",
         "第 3 组；与右侧差 >100mm 时优先信细扫过的那一侧，并在备注写差异"),
        ("最坏情况可见性（贴线边 60mm 能否早看到）", None, "", "", "",
         "第 4 组 Y=±60 时能否看到 ⇒ 直接决定 A02 敢不敢按『贴线边』假设设计"),
        ("中央盲区（Y=0 是否可见）", None, "", "", "",
         "第 4 组 Y=0；不可见说明 15° 改装没达到目的，需要改安装角"),
        ("判侧交叉点（左右谁能先看到）", None, "mm", "", "",
         "第 4 组各行左右读数对比：Y 从 0 往外，哪颗先失去目标 ⇒ 判侧阈值"),
        ("横向覆盖外边界（单侧）", None, "mm", "", "",
         "第 4 组能看到的 |Y| 最大值（+200 仍可见就补测 +280）"),
        ("远限（触发上限）", None, "mm", "", "", "第 5 组 X=1200 是否还有数字（配合 10/01 实测 ≤1.1m）"),
        ("近限（极近兜底阈值）", None, "mm", "", "",
         "第 5 组 X=150 / 100 是否还有数字 ⇒ 校准 AV_STOP_MM(30mm) 与『太近测不到』的边界"),
        ("大障碍对比（宽障碍是否更早看到）", None, "mm", "", "", "第 6 组 vs 第 1 组同 X ⇒ 窄障碍的裕度"),
        ("探头前端 → 车最前端偏移", None, "mm", "", "", "手工量，或从『卷尺刻度 − 探头读数』反算"),
    ]
    for i, row in enumerate(rows, start=3):
        for j, v in enumerate(row, start=1):
            cell = ws.cell(row=i, column=j, value=v)
            cell.font, cell.border = F_BODY, BOX
            cell.alignment = LEFT if j in (1, 6) else CENTER
            if j == 2:
                cell.fill = FILL_FILLIN
    return ws


def main() -> int:
    wb = Workbook()
    wb.remove(wb.active)
    build_help(wb)
    build_table(wb)
    build_manual(wb)
    build_result(wb)
    wb.save(OUT)
    n = sum(len(items) for _, items in ROWS)
    print(f"[OK] {OUT.name}（{OUT.stat().st_size / 1024:.1f} KB）· 点位数 = {n}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
