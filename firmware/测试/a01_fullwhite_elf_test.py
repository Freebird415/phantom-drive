"""Exercise the built ARM ELF: full-white run, reacquisition, finish priority."""
from pathlib import Path
import struct

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC,
                               UC_ARM_REG_CPSR)

elf = Path(__file__).resolve().parents[2] / "firmware/.pio/build/bluepill_f103c8/firmware.elf"
with elf.open("rb") as source:
    image = ELFFile(source)
    symbols = {s.name: s["st_value"] for s in image.get_section_by_name(".symtab").iter_symbols()}
    segments = [(p["p_vaddr"], p.data()) for p in image.iter_segments() if p["p_type"] == "PT_LOAD"]

uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
uc.mem_map(0x08000000, 0x10000)
uc.mem_map(0x20000000, 0x10000)
for address, data in segments:
    uc.mem_write(address, data)

stop, params = 0x0800fff0, 0x2000d000
uc.mem_write(params, struct.pack("<fffi", 21.0, 0.05, 0.68, 22))
now, bits, read_ok = 1000, 0, True
motors = []
names = ("HAL_GetTick", "Gr_ReadDigital", "Tlm_Params", "En_Count", "Log_GetGz",
         "Mo_Set", "Mo_Stop", "Mo_Brake", "Log_Note", "Tlm_SendT", "Tlm_SendEvent",
         "Log_Stop", "Hmi_Beep", "HAL_Delay")
calls = {symbols[name] & ~1: name for name in names if name in symbols}


def signed(value):
    return value if value < 0x80000000 else value - 0x100000000


def hook(engine, address, _size, _user):
    if address == stop:
        engine.emu_stop()
        return
    name = calls.get(address)
    if name is None:
        return
    r0, r1, r2 = (engine.reg_read(reg) for reg in
                  (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2))
    result = None
    if name == "HAL_GetTick":
        result = now
    elif name == "Gr_ReadDigital":
        if read_ok:
            engine.mem_write(r0, bytes([bits ^ 0xff]))
        result = int(read_ok)
    elif name == "Tlm_Params":
        result = params
    elif name in ("En_Count", "Log_GetGz"):
        result = 0
    elif name == "Mo_Set":
        motors.append((now, signed(r0), signed(r1)))
    elif name in ("Mo_Stop", "Mo_Brake"):
        motors.append((now, 0, 0))
    if result is not None:
        engine.reg_write(UC_ARM_REG_R0, result)
    engine.reg_write(UC_ARM_REG_PC, engine.reg_read(UC_ARM_REG_LR))


for address in [stop, *calls]:
    uc.hook_add(UC_HOOK_CODE, hook, begin=address, end=address)


def call(name, *args):
    uc.reg_write(UC_ARM_REG_CPSR, 0x20)
    uc.reg_write(UC_ARM_REG_SP, 0x2000f000)
    uc.reg_write(UC_ARM_REG_LR, stop | 1)
    for reg, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2), args):
        uc.reg_write(reg, value)
    uc.emu_start(symbols[name] | 1, stop, count=100000)
    assert uc.reg_read(UC_ARM_REG_PC) == stop, name


def step(time, gray, ok=True):
    global now, bits, read_ok
    now, bits, read_ok = time, gray, ok
    call("Trace_Step", 1)


uc.mem_write(symbols["s_online"], b"\x01")
call("Trace_StartRun")
call("Trace_SetDrive", 1)
for time in (1000, 4000, 10000):
    step(time, 0)
    assert motors[-1][1:] == (26, 26), (time, motors[-1])
step(10010, 0x18)
assert motors[-1][1:] not in ((0, 0), (26, 26)), motors[-1]
step(10020, 0, False)
assert motors[-1][1:] == (0, 0), "I2C failure must still stop"

# The finish state machine runs before the all-white fallback.
uc.mem_write(symbols["s_fin_armed"], b"\x01")
uc.mem_write(symbols["s_odo_cnt"], struct.pack("<i", 8000))
for time in (10030, 10040, 10050):
    step(time, 0xff)
assert bytes(uc.mem_read(symbols["s_fin"], 1)) == b"\x01", "finish was not armed"
step(10060, 0)
assert motors[-1][1:] == (22, 22), "finish motion must override full-white PWM"
call("Trace_SetDrive", 0)
assert motors[-1][1:] == (0, 0), "manual drive-off must still stop"
print("PASS: 26/26 persists on full white, black resumes A01, gray read failure stops, finish priority intact")
