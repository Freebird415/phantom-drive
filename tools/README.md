# tools —— 上位机与脚本

全部为 Python 脚本，在**仓库根目录**执行。

| 文件 | 作用 |
|---|---|
| `webview.py` + `webui.html` | ⭐ **无线日志网页界面**：`python tools/webview.py` → 浏览器打开 `http://127.0.0.1:8777/`。实时表列（灰度 / err / pwm / state / 编码器 / 陀螺 / 超声波）、超声波俯视图、事件流、按帧号截取导出 |
| `btview.py` | 无线日志（tkinter 版）：实时帧 + 陀螺仪/超声波面板 + 按帧号范围导出片段 |
| `btlog.py` | 无线日志命令行版：自动识别模块 → 订阅 → 解 8B 帧 → CSV |
| `telemetry.py` | 遥测协议解析 + 实验档案（被其它脚本共用）|
| `snapshot.py` | 读**板载快照**（跑车插不了串口时用）：`DUMP` → CSV → 出图 |
| `track.py` | 用快照里的左右轮增量重建实际轨迹 |
| `plot.py` | 把某次实验日志画成曲线 |
| `run.py` | 台面一键实验：编译烧录 → 采集串口 → 存档 → 出图 |
| `shared_log.py` | 生成共享摘要 `logs/共享_*.md`（3~6KB，入库）|
| `flash.py` | 编译 + 烧录（ST-Link 多策略重试、中文诊断、UART 兜底）|
| `自检.py` | 元件自检：I2C 器件 / 快照 + 人配合测按键、蜂鸣器、编码器、电机 |

## 常用命令

```bash
python tools/btview.py                          # 跑车时看实时数据（网页版 webview.py 更推荐）
python tools/snapshot.py                        # 读板载快照并出图
python tools/track.py logs/<某次实验目录>        # 重建轨迹
python tools/btlog.py --help                    # 无线日志命令行用法
python tools/自检.py                            # 动过接线/换板后跑一遍（测电机前会要求架空轮子）
```

数据文件不入库：`logs/` 只放 `共享_*.md` 摘要，原始 CSV 留在本机。
