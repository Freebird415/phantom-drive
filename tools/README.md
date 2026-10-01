# tools —— 上位机与脚本

> **不想敲命令？** 两个更省事的入口：
> - 仓库根目录双击 **`面板.bat`** —— 菜单式（一键实验 / 看串口 / 看日志）
> - VS Code 里 `Ctrl+Shift+P` → **`Tasks: Run Task`** —— 8 个预设任务（在 `firmware/.vscode/tasks.json`）
> 下面写的命令是这两个入口背后跑的东西，等同可用。

## 现在有什么（P01 反馈链路）

| 文件 | 作用 |
|---|---|
| `run.py` | **一键实验**：编译烧录 → 采集串口 → 存档 → 出图（台面调试用）|
| `plot.py` | 把某次实验日志画成曲线图 |
| `telemetry.py` | 协议解析 + 实验档案（被上面两个脚本共用）|
| `snapshot.py` | **读板载快照**（跑车插不了串口）：发 `DUMP` → 存 CSV → 调 `plot.py` 出图 |
| `track.py` | **航位推算重建实际轨迹**（用快照里的左右轮真实增量 `dL/dR` ⇒ 画出走过的形状）|
| `btlog.py` | **BLE 无线日志**（跑车时不用插串口）：自动识别模块 → 订阅 FFE1 → 解 8B 帧 → CSV/图 |
| `btview.py` | **BLE 无线日志（图形界面）**：自己双击「蓝牙日志.bat」→ 开窗口实时看每一帧 + 自动存 CSV；支持「打点」标注时刻 |
| `自检.py` | **元件自检**：自动测 I2C 器件/快照 + 人配合测按键/蜂鸣器/编码器/电机，结果存入 `logs/` |
| `生成字库.py` | 按字表生成 OLED 的子集字库（改字表 → 重跑 → 重新编译）|

## 用法（在仓库根目录执行）

```bash
python tools/run.py --note "过弯抖" --seconds 30          # 完整流程（台面：编译+烧录+采集）
python tools/run.py --note "..." --seconds 20 --no-flash # 只采集
python tools/run.py --ports                              # 查看串口
python tools/plot.py logs/<某次实验目录>                  # 单独出图
```

```bash
# 动过接线/换板/保险丝之后跑一遍（人在车边配合；测电机前会要求确认轮子已架空）
python tools/自检.py
python tools/自检.py --port COM9          # 指定串口
python tools/自检.py --skip-motor         # 只测不动车的部分
```

**跑车（无线，主链路）**：

```bash
# 不想敲命令：双击仓库根目录「蓝牙日志.bat」→ 窗口里点「开始接收」
python tools/btview.py                                     # 同上，图形界面版（推荐给跑车的人）
python tools/btlog.py --note "A4 直线 kp14" --seconds 30   # 命令行版（车通电即自动回传，BTL 默认开）
python tools/btlog.py --scan                              # 看附近有哪些 BLE 设备
python tools/btlog.py --raw --seconds 60                   # 顺手存原始字节，排查用
```
> ⚠️ 实测这类克隆模块**每次连接的速率差很大**（1.5 ~ 34 帧/s 都出现过，33 帧/s ≈ 290 B/s 时基本不丢帧）。
> 所以两个工具都有**会话优选**（连上先试测 6 秒，太稀就重连换会话）；
> 想看 100Hz 全精度数据仍用板载快照（下面这条）。
> **注意**：BLE 帧是**每 3 个采样发 1 帧**（33.3Hz），帧里的 `dL/dR` 只是那 **10ms** 的增量
> —— 想用帧里程求和时**要 ×3**（更准就等锚帧功能，见 `项目记录/经验与踩坑.md`）。

**跑车（插不了串口）用板载快照**：

```bash
# 车：长按 PA12 1 秒 = 记录+发车；再长按 = 停车 + 自动存 Flash（先停车再断电！）
python tools/snapshot.py --note "A4 直线 kp14 base35"     # 读回 → CSV + 出图
python tools/snapshot.py --status                        # 只看板子上记了多少帧
python tools/track.py logs/<某次实验目录>                  # 重建实际轨迹（形状可靠）
```

每轮实验自动存到 `logs/<时间>_<描述>/`：`telemetry.csv`（数据）、`meta.json`（代码版本/参数/端口）、
`note.md`（你的描述+摘要）、`plot.png`（曲线图）——直接把 `note.md` + `plot.png` 发给智能体即可分析。

## 依赖

```bash
python -m pip install pyserial      # matplotlib 一般随 Python 自带
```

## 要求与约定

- 实验日志一律落到 `logs/<时间>_<描述>/`，**logs/ 不入库**（`.gitignore` 已覆盖）
- 每次采集都会自动带上参数快照（`$P` 帧）——没有快照的日志等于"半黑盒"，不要用来下结论
- 板载快照读出的 CSV **列与 `run.py` 的完全一致**（`DUMP` 行格式同 `$T`）⇒ 同一套 `plot.py` 直接画
- `track.py` 依赖快照里的 `dL/dR`（**固件 2026-10-01 起才有**）：旧日志会报错提示；
  **`mm_per_count` 未标定时轨迹长度比例不准，但形状是对的**（先用 `DISTCAL` 标定）
- 采集脚本必须用**限时循环**，不能写 `while True: readline()`（板子 100 Hz 持续输出会永远读不完）
- 协议改动要同步 `docs/遥测协议.md`（改动需双方确认）

## 时间点（P01 三级交付）

| 日期 | 交付 | 状态 |
|---|---|---|
| 9/13 | 阶段 1：一键烧录 + 看数据（100Hz 遥测、存档出图、在线调参命令）| ✅ |
| 9/30 | 阶段 2：真实传感器数据链路跑通 | ✅ |
| 10/1 | 板载快照全链路（脱机跑 → 掉电不丢 → 事后读 → 出图）+ 轨迹重建 | ✅ |
| **10/14** | 阶段 3：PID/状态机数据 + 在线调参全链路（**参数掉电保存待做**）| ⏳ |

- 相关任务：P01
