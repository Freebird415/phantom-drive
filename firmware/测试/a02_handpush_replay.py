"""Run the built A02 hand-push replay in the ARM ELF emulator; no car motion."""
from pathlib import Path
import re

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_MCLASS, UC_MODE_THUMB
from unicorn.arm_const import (
    UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC,
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_SP,
)

elf_path = Path(__file__).resolve().parents[1] / '.pio/build/bluepill_f103c8/firmware.elf'
source_path = Path(__file__).resolve().parents[1] / 'src/app/app_avoid.c'
source_text = source_path.read_text(encoding='utf-8')


def parameter(name):
    """Read the tunable constant so changing a stage duration does not stale the test."""
    match = re.search(r'^#define\s+' + name + r'\s+(\d+)u?', source_text, re.M)
    assert match, name
    return int(match.group(1))


straight_end = 1000 + parameter('A02_STRAIGHT_MS')
left_end = straight_end + parameter('A02_LEFT_MS')
right_end = left_end + parameter('A02_RIGHT_MS')
max_pwm = max(parameter(name) for name in (
    'A02_STRAIGHT_PWM', 'A02_LEFT_L_PWM', 'A02_LEFT_R_PWM',
    'A02_RIGHT_L_PWM', 'A02_RIGHT_R_PWM',
))
with elf_path.open('rb') as source:
    elf = ELFFile(source)
    symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    segments = [(p['p_vaddr'], p.data()) for p in elf.iter_segments() if p['p_type'] == 'PT_LOAD']

uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
uc.mem_map(0x08000000, 0x10000)
uc.mem_map(0x20000000, 0x10000)
for address, data in segments:
    uc.mem_write(address, data)

stop = 0x0800fff0
now = 1000
motors = []
events = []
forbidden = {'Gr_ReadDigital', 'Us_Get', 'En_Count', 'Log_GetGz', 'Trace_Step'}
names = {'HAL_GetTick', 'Mo_Set', 'Mo_Stop', 'Log_Note', 'Tlm_SendT', 'Tlm_SendEvent'}
hooks = {symbols[name] & ~1: name for name in names | (forbidden & symbols.keys())}


def cstr(address):
    value = bytearray()
    while (char := bytes(uc.mem_read(address, 1))) != b'\0':
        value += char
        address += 1
    return value.decode()


def hook(engine, address, _size, _user):
    if address == stop:
        engine.emu_stop()
        return
    name = hooks.get(address)
    if name is None:
        return
    assert name not in forbidden, f'Unexpected sensor/trace call: {name}'
    r0, r1 = engine.reg_read(UC_ARM_REG_R0), engine.reg_read(UC_ARM_REG_R1)
    if name == 'HAL_GetTick':
        engine.reg_write(UC_ARM_REG_R0, now)
    elif name == 'Mo_Set':
        motors.append((now, r0, r1))
    elif name == 'Mo_Stop':
        motors.append((now, 0, 0))
    elif name == 'Tlm_SendEvent':
        events.append((now, cstr(r1)))
    engine.reg_write(UC_ARM_REG_PC, engine.reg_read(UC_ARM_REG_LR))


for address in (stop, *hooks):
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


call('Avoid_Init')
assert call('Avoid_Mode') == 0
call('Avoid_SetDrive', 1)
assert call('Avoid_Mode') == 1
for now in range(1000, right_end + 20, 10):
    call('Avoid_Step')
assert [(t, s) for t, s in events if s.startswith('A02 replay')] == [
    (1000, 'A02 replay straight'),
    (straight_end, 'A02 replay left'),
    (left_end, 'A02 replay right'),
    (right_end, 'A02 replay stop'),
]
assert all(0 <= l <= max_pwm and 0 <= r <= max_pwm for _, l, r in motors)
assert all(l == r for t, l, r in motors if 1100 <= t < straight_end)
assert all(l < r for t, l, r in motors if straight_end + 100 <= t < left_end)
assert all(l > r for t, l, r in motors if left_end + 100 <= t < right_end)
assert all(l == r == 0 for t, l, r in motors if t >= right_end)
assert all(max(abs(b[1] - a[1]), abs(b[2] - a[2])) <= 2
           for a, b in zip(motors, motors[1:]) if b[0] < right_end and b[0] != a[0])
assert call('Avoid_Mode') == 1, 'A01 must stay disabled after automatic stop'
call('Avoid_SetDrive', 0)
assert call('Avoid_Mode') == 0 and motors[-1][1:] == (0, 0)
print('PASS: timed stages, PWM slew, stop latch, no sensors')
