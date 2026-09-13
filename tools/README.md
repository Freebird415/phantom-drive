# tools —— 上位机与脚本

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

## 约定

- 协议改动要同步 `docs/遥测协议.md`（改动需双方确认）
- 相关任务：P01
