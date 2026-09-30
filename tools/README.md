# tools —— 上位机与脚本

> **不想敲命令？** 两个更省事的入口：
> - 仓库根目录双击 **`面板.bat`** —— 菜单式（一键实验 / 看串口 / 看日志）
> - VS Code 里 `Ctrl+Shift+P` → **`Tasks: Run Task`** —— 8 个预设任务（在 `firmware/.vscode/tasks.json`）
> 下面写的命令是这两个入口背后跑的东西，等同可用。

## 现在有什么（P01 反馈链路）

| 文件 | 作用 |
|---|---|
| `run.py` | **一键实验**：编译烧录 → 采集串口 → 存档 → 出图 |
| `plot.py` | 把某次实验日志画成曲线图 |
| `telemetry.py` | 协议解析 + 实验档案（被上面两个脚本共用） |

## 用法（在仓库根目录执行）

```bash
python tools/run.py --note "过弯抖" --seconds 30          # 完整流程
python tools/run.py --note "..." --seconds 20 --no-flash # 只采集
python tools/run.py --ports                              # 查看串口
python tools/plot.py logs/<某次实验目录>                  # 单独出图
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
- 采集脚本必须用**限时循环**，不能写 `while True: readline()`（板子 100 Hz 持续输出会永远读不完）
- 协议改动要同步 `docs/遥测协议.md`（改动需双方确认）

## 时间点（P01 三级交付）

| 日期 | 交付 |
|---|---|
| 9/13 已完成 | 阶段 1：一键烧录 + 看数据（100Hz 遥测、存档出图、在线调参命令） |
| 9/28 起 | 面包板 demo 到手后，接真实传感器读数、落盘、出图（可逐模块做） |
| **9/30** | 阶段 2：真实传感器数据链路跑通 |
| **10/14** | 阶段 3：PID/状态机数据 + 在线调参全链路（配合实车调试） |

- 相关任务：P01
