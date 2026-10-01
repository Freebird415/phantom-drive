# firmware/include —— 这个目录为什么是空的

## 一句话

PlatformIO 的默认工程结构约定有一个 `include/` 目录，VS Code 生成 IntelliSense 配置时
会把它加进头文件搜索路径。**我们不使用这个约定**，但目录不存在时 VS Code 的
"问题"面板会报两条黄三角：

```
⚠ 无法找到 "…/firmware/include"
```

**这两条不影响编译、烧录、调试**（只是代码补全少扫一个不存在的目录），
但为了让"问题"面板保持干净、不让新同学误以为出错，这里保留一个空目录。

## 本工程的头文件放在哪

```
firmware/src/
  bsp/    板级        bsp_board.h
  drv/    器件驱动    drv_ultrasonic.h / drv_grayscale.h / drv_oled.h /
                     drv_motor.h / drv_encoder.h / drv_hmi.h / drv_mpu6050.h
  app/    应用层      app_telemetry.h / app_trace.h / app_avoid.h
```

**规则：一个模块的头文件和源文件放同一个目录**（`drv_xxx.c` 和 `drv_xxx.h` 并排），
不集中到 `include/`。这样找"某个驱动的接口"只需打开一个目录。

分层规则与接口清单见 `firmware/驱动接口说明.md`。
