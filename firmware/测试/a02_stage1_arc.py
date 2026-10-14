"""Exercise the built ARM A02 test state machine without moving the car.

Run after ``pio run -d firmware`` with Unicorn and pyelftools installed.
These assertions cover control flow and motor commands, not physical radius.
"""
from pathlib import Path
import struct

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import (
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_SP,
    UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_CPSR,
)

elf_path = Path(__file__).resolve().parents[1] / '.pio/build/bluepill_f103c8/firmware.elf'
with elf_path.open('rb') as source:
    elf = ELFFile(source)
    symbols = {sym.name: sym['st_value'] for sym in elf.get_section_by_name('.symtab').iter_symbols()}
    segments = [(part['p_vaddr'], part.data()) for part in elf.iter_segments()
                if part['p_type'] == 'PT_LOAD']

uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
uc.mem_map(0x08000000, 0x10000)
uc.mem_map(0x20000000, 0x10000)
for address, data in segments:
    uc.mem_write(address, data)

stop = 0x0800fff0
now = 1000
bits = 0x18
gray_ok = True
motors = []
events = []
calls = {symbols[name] & ~1: name for name in (
    'HAL_GetTick', 'Mo_Set', 'Mo_Stop', 'Mo_Brake', 'Gr_ReadDigital',
    'Log_Note', 'Tlm_SendT', 'Tlm_SendEvent',
)}


def cstr(address):
    result = bytearray()
    while True:
        value = bytes(uc.mem_read(address, 1))[0]
        if not value:
            return result.decode()
        result.append(value)
        address += 1


def hook(engine, address, size, _user):
    if address == stop:
        engine.emu_stop()
        return
    name = calls.get(address)
    if name is None:
        return
    r0 = engine.reg_read(UC_ARM_REG_R0)
    r1 = engine.reg_read(UC_ARM_REG_R1)
    if name == 'HAL_GetTick':
        engine.reg_write(UC_ARM_REG_R0, now)
    elif name == 'Gr_ReadDigital':
        if gray_ok:
            engine.mem_write(r0, bytes([bits ^ 0xff]))
        engine.reg_write(UC_ARM_REG_R0, int(gray_ok))
    elif name == 'Mo_Set':
        motors.append((r0, r1))
    elif name in ('Mo_Stop', 'Mo_Brake'):
        motors.append((0, 0))
    elif name == 'Tlm_SendEvent':
        events.append(cstr(r1))
    engine.reg_write(UC_ARM_REG_PC, engine.reg_read(UC_ARM_REG_LR))


for address in [stop, *calls]:
    uc.hook_add(UC_HOOK_CODE, hook, begin=address, end=address)


def call(name, *args):
    uc.reg_write(UC_ARM_REG_CPSR, 0x20)
    uc.reg_write(UC_ARM_REG_SP, 0x2000f000)
    uc.reg_write(UC_ARM_REG_LR, stop | 1)
    for register, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2), args):
        uc.reg_write(register, value)
    uc.emu_start(symbols[name] | 1, stop, count=100000)
    assert uc.reg_read(UC_ARM_REG_PC) == stop, name
    return uc.reg_read(UC_ARM_REG_R0)


def step(time_ms, gray_bits=0x18, read_ok=True):
    global now, bits, gray_ok
    now, bits, gray_ok = time_ms, gray_bits, read_ok
    call('Avoid_Step')
    assert call('Avoid_Mode') == 1, 'A01 must not own the button-started test'


call('Avoid_Init')
call('Avoid_SetDrive', 1)
assert call('Avoid_Mode') == 1, 'A02 must own the very first control tick'
step(1100)
assert motors[-1] == (0, 0)
step(3000)
for tick in range(3010, 3170, 10):
    step(tick)
assert motors[-1] == (16, 16), motors[-1]
step(4000)
for tick in range(4010, 4050, 10):
    step(tick)
assert motors[-1][0] < motors[-1][1], motors[-1]
step(4500, 0x00)
step(4800, 0x00)
for tick in range(4810, 4860, 10):
    step(tick, 0x00)
assert motors[-1][0] > motors[-1][1], motors[-1]

# Gray read failure and more than 10 seconds of all white must not auto-stop.
step(15000, 0x00, read_ok=False)
step(20000, 0x00)
assert motors[-1] == (14, 10), motors[-1]
assert not any('timeout' in event or 'failed' in event for event in events)
# 见黑 -> 交还 A01：A02 松手，Avoid_Mode 变 0，不再由 A02 循迹
now, bits, gray_ok = 20010, 0x01, True
call('Avoid_Step')
assert 'A02 TEST line found; handoff to A01' in events
assert call('Avoid_Mode') == 0, 'handoff must release control to A01'
# A02 已放手：后续 Avoid_Step 应直接返回、不改电机
call('Avoid_Step')
assert call('Avoid_Mode') == 0

# 手动重新武装：下一趟仍进入同一测试（不发车，只验证入口）
call('Avoid_SetDrive', 1)
assert call('Avoid_Mode') == 1
step(21000)
assert motors[-1] == (0, 0)
print('PASS: exclusive button entry, 2s wait, 1s gray follow (x2), left then right (x2), black -> handoff to A01, manual restart')
