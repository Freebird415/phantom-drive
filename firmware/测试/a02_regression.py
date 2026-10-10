"""Run actual ARM firmware recovery code with mocked hardware; never write logs.

Uses the locally built ELF, not a Python translation of the algorithm.
This checks control decisions, not the car's physical trajectory or clearance.

From the repository root: pio run -d firmware, then run this file with Python.
Python dependencies: unicorn, pyelftools (install in the development venv).
"""
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_CPSR

ROOT = Path(__file__).resolve().parents[2]
elf_path = ROOT / 'firmware/.pio/build/bluepill_f103c8/firmware.elf'
with elf_path.open('rb') as f:
    elf = ELFFile(f)
    symbols = {s.name: (s['st_value'], s['st_size']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
    # Local names such as s_steer_out also occur in A01; select A02's copy.
    source = ''
    for s in elf.get_section_by_name('.symtab').iter_symbols():
        if s['st_info']['type'] == 'STT_FILE':
            source = s.name
        elif source == 'app_avoid.c' and s['st_info']['bind'] == 'STB_LOCAL':
            symbols[s.name] = (s['st_value'], s['st_size'])
    segments = [(p['p_vaddr'], p.data()) for p in elf.iter_segments() if p['p_type'] == 'PT_LOAD']

u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
u.mem_map(0x08000000, 0x10000)
u.mem_map(0x20000000, 0x10000)
for address, data in segments:
    u.mem_write(address, data)
events = []
motors = []
STOP = 0x0800fff0

def put(name, value, fmt=None):
    addr, size = symbols[name]
    if fmt is None:
        fmt = 'f' if isinstance(value, float) else {1: 'B', 2: 'H', 4: 'I'}[size]
    u.mem_write(addr, struct.pack('<' + fmt, value))

def get(name, fmt=None):
    addr, size = symbols[name]
    fmt = fmt or {1: 'B', 2: 'H', 4: 'I'}[size]
    return struct.unpack('<' + fmt, bytes(u.mem_read(addr, size)))[0]

def cstr(addr):
    out = bytearray()
    while True:
        b = bytes(u.mem_read(addr, 1))
        if b == b'\0':
            return out.decode()
        out += b
        addr += 1

def signed(n):
    return n if n < 0x80000000 else n - 0x100000000

hooks = {symbols[n][0] & ~1: n for n in ['Mo_Set', 'Mo_Brake', 'Tlm_SendEvent', 'Log_Note', 'Tlm_SendT']}

def hook(uc, address, size, user):
    if address == STOP:
        uc.emu_stop()
        return
    name = hooks.get(address)
    if not name:
        return
    if name == 'Mo_Set':
        motors.append((signed(uc.reg_read(UC_ARM_REG_R0)), signed(uc.reg_read(UC_ARM_REG_R1))))
    elif name == 'Mo_Brake':
        motors.append((0, 0))
    elif name == 'Tlm_SendEvent':
        events.append(cstr(uc.reg_read(UC_ARM_REG_R1)))
    uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

for address in [STOP, *hooks]:
    u.hook_add(UC_HOOK_CODE, hook, begin=address, end=address)

def call(ms, bits):
    u.reg_write(UC_ARM_REG_CPSR, 0x20)
    u.reg_write(UC_ARM_REG_SP, 0x2000f000)
    u.reg_write(UC_ARM_REG_LR, STOP | 1)
    u.reg_write(UC_ARM_REG_R0, ms)
    u.reg_write(UC_ARM_REG_R1, bits)
    u.reg_write(UC_ARM_REG_R2, bits.bit_count())
    u.emu_start(symbols['av_recover'][0] | 1, STOP, count=100000)
    assert u.reg_read(UC_ARM_REG_PC) == STOP, 'Execution did not finish'

def reset(yaw=0.0, err=0.0, away=-1):
    for name in ['s_center_ms', 's_line_ms']:
        put(name, 0)
    put('s_phase', 6)  # AV_RECOVER
    put('s_phase_ms', 1000)
    put('s_dt_ms', 10)
    put('s_away', away & 0xff)
    for name, value in [('s_yaw_deg', yaw), ('s_yaw_rate', 0.0), ('s_steer_out', 0.0),
                        ('s_line_err', err), ('s_line_d', 0.0), ('s_trip_mm', 0.0),
                        ('s_phase_mm', 0.0), ('s_line_trip', 0.0)]:
        put(name, value)
    motors.clear()
    events.clear()
    assert get('s_steer_out', 'f') == 0.0

# A locally centered curve must not be rejected just because entry-relative yaw >18 degrees.
for yaw in (-60.0, 60.0):
    reset(yaw=yaw)
    for ms in range(1010, 1370, 10):
        call(ms, 0x18)
    assert get('s_phase') == 0 and 'AVOID line recovered' in events
    assert all(l == r and l <= 12 for l, r in motors)
print('PASS: curve-local centering and 350ms handoff, both directions')

# Replay the critical edge -> white pattern from 12:01, with mirrored direction.
for bits, sign in [(0xe0, 1), (0x07, -1)]:
    reset(yaw=45.0 * sign)
    for ms in (1010, 1020, 1030):
        call(ms, bits)
    call(1040, 0)
    assert get('s_phase') == 6
    assert all((l - r) * sign > 0 and 0 <= l <= 18 and 0 <= r <= 18 for l, r in motors), (sign, motors)
    call(1200, 0)
    assert get('s_phase') == 7 and motors[-1] == (0, 0)
    assert 'AVOID local line lost' in events
print('PASS: edge-loss search preserves direction, then brakes at timeout')

reset()
call(1010, 0xf0)  # Four adjacent sensors: oblique-line capture, not ignored.
assert get('s_line_ms') == 1010 and motors[-1][0] > motors[-1][1]
put('s_trip_mm', 26.0)
call(1020, 0)
assert get('s_phase') == 7 and motors[-1] == (0, 0)
print('PASS: four-sensor capture and nominal 25mm lost-line travel bound')

for bits in (0xff, 0x81, 0):
    reset()
    call(1010, bits)
    assert get('s_phase') == 7 and 'AVOID line recovered' not in events
print('PASS: wide/disjoint/white cannot cause false handoff')

reset()
for ms in range(1010, 1090, 10):
    call(ms, 0x18)
call(1090, 0)
for ms in range(1100, 1450, 10):
    call(ms, 0x18)
assert get('s_phase') == 6
call(1450, 0x18)
assert get('s_phase') == 0
print('PASS: handoff requires continuous centering, not accumulated sightings')

reset()
put('s_yaw_rate', 100.0)
for ms in range(1010, 1400, 10):
    call(ms, 0x18)
assert get('s_phase') == 6 and 'AVOID line recovered' not in events
print('PASS: rapid rotation blocks premature handoff')

reset()
call(1010, 0x18)
call(5600, 0x18)
assert get('s_phase') == 7 and 'AVOID centering failed' in events
print('PASS: overall centering timeout still brakes')
