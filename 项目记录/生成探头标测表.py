# -*- coding: utf-8 -*-
"""生成《15° 探头几何标测 · 记录表》Excel（给毕烨恺现场手抄用）。

用法：  python 生成探头标测表.py        # 覆盖生成 探头几何标测_记录表.xlsx

为什么要用表格而不是日志：本次所有点位都是**静止**测量（车不动、障碍物放好就不动），
读数在蓝牙面板上是“瞬时值” ⇒ 直接读 + 抄表最快、最不含糊；
日志留给“跑起来才有意义”的测试（可选抽查 1~2 条）。

结构（4 个 sheet）：
    说明      现场执行细则（**唯一操作口径**，表格自带，不用再翻别的文档）
    记录表    预填好的点位清单，只填 4 个读数列 + 备注 + 照片号
    手工量    卷尺量的 3 个数（探头偏移 / 探头高度 / 线宽）
    结论      目标值（由组长/AI 填，现场不用管）
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


# ---------------------------------------------------------------- 点位清单
SMALL = "小障碍 60×60×150（阿凡提）"
BIG = "大障碍 150×100×150（莫高窟）"
LINE_EDGE = "内边贴线（离车中轴 10）"  # 组标题用

XS_FAR = [1200, 1100, 1000, 900, 800, 700, 600, 500, 450, 400, 350, 300, 250, 200]
XS_BIG = [1200, 1000, 800, 600, 500, 400, 300]
YS_SIDE = [0, 40, -40, 80, -80, 120, -120, 160, -160, 200, -200, 250, -250]

ROWS: list[tuple[str, list[tuple]]] = [
    ("第 1 组 ⭐ 右侧 · 小障碍 · 贴线边 · 从远往近（必做，先做这组）",
     [(SMALL, "右侧", x, LINE_EDGE, 10, x in (700, 600, 500, 400)) for x in XS_FAR]),

    ("第 2 组 ⭐ 右侧 · 小障碍 · 细扫（做完第 1 组再回来：X 抄第 1 组『首次出数字』附近 ±150mm，每档 50mm）",
     [(SMALL, "右侧", None, LINE_EDGE, 10, True) for _ in range(6)]),

    ("第 3 组 ⭐ 右侧 · 大障碍 · 贴线边（验『障碍越宽越早看到』）",
     [(BIG, "右侧", x, LINE_EDGE, 10, x in (800, 600, 500)) for x in XS_BIG]),

    ("第 4 组 ⭐ 左侧 · 小障碍 · 贴线边 · 从远往近",
     [(SMALL, "左侧", x, LINE_EDGE, -10, x in (700, 600, 500, 400)) for x in XS_FAR]),

    ("第 5 组 ⭐ 左侧 · 小障碍 · 细扫（同第 2 组，换左边）",
     [(SMALL, "左侧", None, LINE_EDGE, -10, True) for _ in range(6)]),

    ("第 6 组 ⭐ 左侧 · 大障碍 · 贴线边",
     [(BIG, "左侧", x, LINE_EDGE, -10, x in (800, 600, 500)) for x in XS_BIG]),

    ("第 7 组 ⭐ 正前方 / 横向覆盖（X 固定 400；这组的 Y 是『障碍中心』对车中轴，不是内边）",
     [(SMALL, "中央" if y == 0 else ("右侧" if y > 0 else "左侧"), 400, "中心对中轴", y, abs(y) <= 80)
      for y in YS_SIDE]),

    ("第 8 组 ⭐ 最近距离（右侧 · 小障碍 · 贴线边，看多近还测得到）",
     [(SMALL, "右侧", x, LINE_EDGE, 10, True) for x in (150, 100, 50)]),

    ("第 9 组 抽查重复（把上面最关键的 3 个点再摆一遍：右侧首检出那点 / 正前中央 / 右侧大障碍首检出）",
     [(SMALL, "自填", None, "自填", None, True) for _ in range(3)]),
]


# ---------------------------------------------------------------- sheet：说明
def build_help(wb):
    ws = wb.create_sheet("说明")
    ws.column_dimensions["A"].width = 4
    ws.column_dimensions["B"].width = 112
    _title(ws, "15° 探头几何标测 · 现场说明（毕烨恺 · 一人可独立完成）", 2)

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

    sec("0. 一句话")
    bullet("把 A02 现在用的『名义探头几何』换成实测值。车不动、障碍物放好就不动 ⇒ "
           "直接在蓝牙面板上读数字、抄进『记录表』即可，不用开日志。")
    bullet("要得到 4 个数：①左右探头首次看到障碍的距离 ②最近能测到多近 "
           "③正前/侧向覆盖（有没有盲区）④探头前端 → 车最前端的偏移。")
    bullet("时间：必做（第 1、3、4、6、7、8 组）约 15 分钟；第 2、5、9 组各 +2 分钟；共约 20 分钟。")

    sec("1. 准备")
    bullet("① 车静置在直线段上（桌面或赛道直线都行），车头朝前，全程别碰车；上电，3V3 ≈ 3.3V。")
    bullet("② 电脑双击「蓝牙日志.bat」→ 等状态栏显示「蓝牙已连接」+ 有帧率（本轮不点开始接收、不用监测）。")
    bullet("③ 卷尺沿车头方向铺开，0 刻度对齐『车最前端』，铺好之后别再碰。")
    bullet("④ 障碍物：60×60×150（阿凡提）与 150×100×150（莫高窟）；没实物就用立起来的厚书（实心、够高）。")
    bullet("⑤ 笔记本放在能看见、又不挡探头的位置：摆好障碍物后人是回到电脑前读数的（障碍物不动，读数不会跑）。")
    bullet("⑥ 左右定义：沿车前进方向看，左手边 = 左探头那侧。")

    sec("2. 每个点位的动作（4 步，所有行都一样）")
    bullet("① 按表格的 X（从车最前端量）摆好障碍物 → ② 等 2 秒 → ③ 读一次，记『左①/右①』→ "
           "④ 再等 2 秒 → 读一次，记『左②/右②』。")
    bullet("⚠️ 面板走蓝牙（约每秒 2~4 次刷新）⇒ 不等到 2 秒就会读到上一个位置的值。"
           "两个数不一样时把差值写进备注，以 ② 为准。")
    bullet("⚠️ 没有数字就照实写：-1 / -2（没测到，正常）· -5（太近或太远）· -6（被限流跳过，正常）。"
           "不要写 0，也不要留空。")
    bullet("⚠️ 从远往近摆（表格已按这个顺序排好）：万一某次没刷新，数值会偏大偏远，一眼能看出来。")

    sec("3. 怎么算『看见了』")
    bullet("右侧数值里出现有效距离（正数，且与卷尺刻度大致对得上）= 右探头看见它了；左侧同理。")
    bullet("同一行左右都出现数字 ⇒ 两颗都看见了（通常先出现靠障碍物那一侧的那颗）。")

    sec("4. 出错 / 漏了怎么办（这一节就是表格的容错设计，不用怕）")
    bullet("抄错一个数：回到那个位置重读两遍，覆盖原格。")
    bullet("某一组整组没做或做废：跳过即可，备注写『废』——左右互为镜像、大小障碍是两条曲线，"
           "我会用其余组兜住。")
    bullet("时间不够：优先做带 ⭐ 的组（第 1、3、4、6、7、8 组）；第 2、5、9 组是加精度/抽查，可以不做。")

    sec("5. 做完怎么给组长")
    bullet("把这个 xlsx 直接发微信给组长即可（不用 git、不用打包日志）。")
    bullet("可选：每个组开工拍一张照片（车 + 障碍物 + 卷尺刻度同框），文件名用组号（如 A1.jpg）"
           "—— 万一哪行抄错，照片能救。", F_NOTE)
    bullet("可选（想留原始证据）：把『第 7 组 Y=0 正前中央』和『第 1 组首次出数字那点』"
           "各开一条 10 秒日志留档。", F_NOTE)

    sec("6. 表格怎么读")
    bullet("「记录表」：黄色格子 = 要你填；每组有一行分组说明（组内顺序别跳）。")
    bullet("「手工量」：卷尺量 3 个数，顺手填了（不填也能从记录表反算）。")
    bullet("「结论」：不用管，组长/AI 填。")
    return ws


# ---------------------------------------------------------------- sheet：记录表
def build_table(wb):
    ws = wb.create_sheet("记录表")
    head = ["序", "障碍物", "侧", "位置 X\n(mm，从车最前端)", "横向 Y\n(内边/中心，mm)",
            "左 读数①\n(mm)", "右 读数①\n(mm)", "左 读数②\n(mm)", "右 读数②\n(mm)",
            "备注（跳动 / 无(-1) / 无(-2) / 废）", "照片#"]
    widths = [5, 26, 7, 13, 16, 11, 11, 11, 11, 30, 8]
    _title(ws, "15° 探头几何标测 · 记录表（黄色格子要填；每个点位读两次，中间等 2 秒）", len(head))
    _head(ws, 2, head, widths)
    ws.freeze_panes = "A3"

    r, n = 3, 0
    for gtext, items in ROWS:
        _group(ws, r, gtext, len(head))
        r += 1
        for (obst, side, x, ykind, y, key) in items:
            n += 1
            if y is None:
                ytxt = ykind                                   # 自填组
            elif ykind == LINE_EDGE:
                ytxt = f"{y}（内边）"                          # 贴线边：Y = 障碍内边到车中轴
            else:
                ytxt = f"{y}（中心）"                          # 第 7 组：Y = 障碍中心到车中轴
            vals = [n, obst, side, x if x is not None else "自填", ytxt,
                    None, None, None, None, None, None]
            for i, v in enumerate(vals, start=1):
                c = ws.cell(row=r, column=i, value=v)
                c.font, c.border = F_BODY, BOX
                c.alignment = LEFT if i in (2, 10) else CENTER
                if i in (6, 7, 8, 9, 10, 11):
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
    head = ["项", "数值", "单位", "怎么量"]
    _title(ws, "卷尺/目测的 3 个数（顺手填；探头偏移也能从记录表反算）", len(head))
    _head(ws, 2, head, [30, 12, 8, 78])
    rows = [
        ("探头前端 → 车最前端", None, "mm", "卷尺从车最前端量到超声探头的前表面（两侧各量一次取平均）"),
        ("探头离地高度", None, "mm", "车放地面/桌面上，量探头中心离地高度（判断障碍物要多高才挡得住）"),
        ("循迹线宽", None, "mm", "量赛道上黑线宽度（确认是否 20mm，用于『贴线边』换算）"),
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
    head = ["目标", "结果", "单位", "置信度", "依据（记录表行号）", "备注"]
    _title(ws, "目标值 —— 现场不用填，组长/AI 从「记录表」算出来写这里", len(head))
    _head(ws, 2, head, [30, 12, 8, 10, 24, 62])
    rows = [
        ("右侧探头 · 首次检出距离", None, "mm", "", "", "贴线边小障碍：第一次出现有效读数的那个 X"),
        ("左侧探头 · 首次检出距离", None, "mm", "", "", "同上，左侧"),
        ("最近可测距离", None, "mm", "", "", "再往近就掉读数/报 -1、-2 的那个 X"),
        ("中央盲区（Y=0 处是否可见）", None, "", "", "", "应『可见』；不可见说明 15° 改装没达到目的"),
        ("横向覆盖（单侧可测范围）", None, "mm", "", "", "X=400 时 Y 从 0 到多少还能检测到"),
        ("探头前端 → 车最前端偏移", None, "mm", "", "", "手工量，或从『卷尺刻度 − 探头读数』反算"),
        ("大障碍（150×100）首次检出距离", None, "mm", "", "", "与小障碍对比 ⇒ 反推探头方位覆盖"),
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
    print(f"[OK] {OUT.name}（{OUT.stat().st_size / 1024:.1f} KB）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
