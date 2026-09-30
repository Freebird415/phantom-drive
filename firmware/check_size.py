# -*- coding: utf-8 -*-
"""编译后体积检查：固件不得压进【板载快照保留区】

板载快照占 Flash 最后 10KB（0x0800D800 = 55296 字节）=> 固件必须 < 55296。
超了会互相覆盖（固件跑飞 / 快照被毁），而且【编译不会报警】—— 所以这里拦一道。
背景见 firmware/src/app/app_log.h

注意：输出只用 ASCII（Windows 控制台是 GBK，emoji 会 UnicodeEncodeError）
"""
Import("env")  # noqa: F821  (PlatformIO 注入)

LIMIT = 0x0800D800 - 0x08000000          # 55296 字节 = 54KB（快照保留区起点：最后 10KB 归快照）


def check_size(source, target, env):  # noqa: ARG001
    import os
    path = env.subst("$BUILD_DIR/${PROGNAME}.bin")
    if not os.path.exists(path):
        return
    size = os.path.getsize(path)
    if size > LIMIT:
        raise SystemExit(
            "\n[FAIL] 固件体积越界：%d 字节 > 板载快照区起点 %d 字节（超 %d 字节）\n"
            "   => 会把快照区覆盖掉。请：\n"
            "      1) 精简代码（别用 atof/atoi 这类会拉进大块库的函数）\n"
            "      2) 或调小 LOG_MAX_FRAMES / 减少保留页数（firmware/src/app/app_log.h）\n"
            % (size, LIMIT, size - LIMIT))
    print("\n[OK] size check: firmware.bin = %d bytes, %d bytes left before snapshot area @0x%08X (%.1f%% margin)\n"
          % (size, LIMIT - size, 0x08000000 + LIMIT, (LIMIT - size) * 100.0 / LIMIT))


# 挂到【链接产物】上，而不是 buildprog 这个目标名 —— 挂 buildprog 时 `pio run` 根本不会触发它
# （实测：只跑 pio run / -t buildprog 都不打印，等于守门没生效）。
# 挂到 .bin 节点后，每次重新链接都会跑一次。
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", check_size)  # noqa: F821
