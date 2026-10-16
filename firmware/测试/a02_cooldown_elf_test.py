"""Offline ARM ELF check: ultrasonic trigger, A01 handoff and cooldown."""
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

stop = 0x0800fff0
left_dev, right_dev, live = 0x2000d000, 0x2000d100, 0x2000d200
uc.mem_write(live, bytes([0, 0, 22, 22, 1, 0, 0, 0]))
now, bits = 1000, 0x18
motors, events = [], []
names = ("HAL_GetTick", "Gr_ReadDigital", "Us_Get", "Trace_GetDrive",
         "Trace_Online", "Log_LiveFrame", "Mo_Set", "Mo_Stop",
         "Tlm_SendEvent", "Log_Note", "Tlm_SendT", "Trace_SetSeek", "Log_Stop")
calls = {symbols[name] & ~1: name for name in names}


def signed(value):
    return value if value < 0x80000000 else value - 0x100000000


def cstr(address):
    data = bytearray()
    while (part := bytes(uc.mem_read(address, 1))) != b"\0":
        data += part
        address += 1
    return data.decode()


def hook(engine, address, _size, _user):
    if address == stop:
        engine.emu_stop()
        return
    name = calls.get(address)
    if name is None:
        return
    r0, r1 = engine.reg_read(UC_ARM_REG_R0), engine.reg_read(UC_ARM_REG_R1)
    result = None
    if name == "HAL_GetTick":
        result = now
    elif name == "Gr_ReadDigital":
        engine.mem_write(r0, bytes([bits ^ 0xff]))
        result = 1
    elif name == "Us_Get":
        result = right_dev if r0 else left_dev
    elif name in ("Trace_GetDrive", "Trace_Online"):
        result = 1
    elif name == "Log_LiveFrame":
        result = live
    elif name == "Mo_Set":
        motors.append((now, signed(r0), signed(r1)))
    elif name == "Mo_Stop":
        motors.append((now, 0, 0))
    elif name == "Tlm_SendEvent":
        events.append((now, cstr(r1)))
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
    return uc.reg_read(UC_ARM_REG_R0)


def step(time, gray, left_mm=0, right_mm=0, left_stamp=0, right_stamp=0):
    global now, bits
    now, bits = time, gray
    for address, mm, stamp in ((left_dev, left_mm, left_stamp),
                               (right_dev, right_mm, right_stamp)):
        uc.mem_write(address + 20, struct.pack("<i", mm))
        uc.mem_write(address + 28, struct.pack("<I", stamp))
    call("Avoid_Step")


call("Avoid_Init")
for side in ("left", "right"):
    call("Avoid_SetDrive", 1)
    near_l, near_r = (0, 280) if side == "left" else (280, 0)
    step(1000, 0x18, 600 if near_l else 0, 600 if near_r else 0,
         1000 if near_l else 0, 1000 if near_r else 0)
    assert call("Avoid_Mode") == 0, side
    step(1100, 0x18, near_l, near_r, 1100 if near_l else 0,
         1100 if near_r else 0)
    assert call("Avoid_Mode") == 1, side
    before = len(events)
    for time in range(1110, 1600, 10):
        # Right case stays black throughout: no hidden full-white prerequisite.
        # Even eight black channels at 1590 must remain masked.
        gray = 0xff if time == 1590 else (0x18 if side == "right" or time < 1200 else 0)
        step(time, gray, near_l, near_r, 1100 if near_l else 0,
             1100 if near_r else 0)
    assert not any("straighten" in text for _, text in events[before:]), side
    if side == "left":
        # Center pair first, then expand outward to six distinct channels.
        sequence = ((1600, 0x18), (1610, 0x3c), (1620, 0x7e))
        align_time = 1620
    else:
        # Five channels followed by all-white must not combine with next stroke.
        # The next six arrive from the edge, in reverse order from the left case.
        sequence = ((1600, 0x1f), (1610, 0x00), (1620, 0x01),
                    (1630, 0x03), (1640, 0x0f), (1650, 0x3f))
        align_time = 1650
    for time, gray in sequence:
        step(time, gray, near_l, near_r, 1100 if near_l else 0,
             1100 if near_r else 0)
        assert ((time, "A02 straighten on line") in events[before:]) == (time == align_time), side
    for time in range(align_time + 10, align_time + 440, 10):
        # Seeing black during the 400ms align does not hand off early.
        gray = 0xff if time == align_time + 200 else 0
        step(time, gray, near_l, near_r, 1100 if near_l else 0,
             1100 if near_r else 0)
    assert (align_time + 430, "A02 capture line") in events[before:], (side, events[before:])
    peak = [entry for entry in motors if align_time + 80 <= entry[0] <= align_time + 300]
    if side == "left":
        assert any(l < 0 and r > 0 for _, l, r in peak), (side, peak)
    else:
        assert any(l > 0 and r < 0 for _, l, r in peak), (side, peak)
    assert call("Avoid_Mode") == 1, side
    assert motors[-1][1:] == ((10, 28) if side == "left" else (28, 10)), (side, motors[-1])
    step(align_time + 440, 0)
    assert call("Avoid_Mode") == 1, side
    step(align_time + 450, 0x80 if side == "left" else 0x01)
    assert call("Avoid_Mode") == 1, (side, events[before:])
    assert (align_time + 450, "A02 search black; keep arc") in events[before:], side
    step(align_time + 690, 0)
    assert call("Avoid_Mode") == 1 and motors[-1][1:] == ((10, 28) if side == "left" else (28, 10)), side
    step(align_time + 700, 0)
    assert call("Avoid_Mode") == 0, (side, events[before:])
    assert (align_time + 700, "A02 search done; A01 handoff") in events[before:], side

call("Avoid_SetDrive", 1)
step(3000, 0x18, 0, 600, 0, 3000)
step(3100, 0x18, 0, 280, 0, 3100)
assert call("Avoid_Mode") == 1
step(3500, 0)
before = len(events)
step(3600, 0x07)
for time in range(3610, 3730, 10):
    step(time, 0x01)
step(3730, 0x38)
assert not any("straighten" in text for _, text in events[before:]), "older channels expired from sliding window"
step(8500, 0)
assert call("Avoid_Mode") == 1 and motors[-1][1:] == (0, 0)
assert any("return timeout stop" in text for _, text in events)

# Start searching just before the old five-second return limit. The search
# must still receive its own full four seconds, then stop if no black appears.
call("Avoid_SetDrive", 1)
step(9000, 0x18, 0, 600, 0, 9000)
step(9100, 0x18, 0, 280, 0, 9100)
step(9500, 0)
step(14000, 0xff)
assert (14000, "A02 straighten on line") in events
step(14430, 0)
assert (14430, "A02 capture line") in events
step(14500, 0)
assert call("Avoid_Mode") == 1, "old return timeout must not shorten search"
step(18920, 0)
assert call("Avoid_Mode") == 1
step(18930, 0)
assert call("Avoid_Mode") == 1 and motors[-1][1:] == (0, 0)
assert (18930, "A02 search timeout stop") in events

# One new, valid near reading is sufficient; stale/invalid values are not.
call("Avoid_SetDrive", 1)
step(19000, 0x18, 0, -2, 0, 19000)
assert call("Avoid_Mode") == 0
step(19010, 0x18, 0, 280, 0, 19000)
assert call("Avoid_Mode") == 0, "same cached echo must not trigger"
step(19200, 0x18, 0, 280, 0, 19200)
assert call("Avoid_Mode") == 1, "one new near reading must trigger"

call("Avoid_SetDrive", 1)
step(20000, 0x18, 280, 0, 20000, 0)
assert call("Avoid_Mode") == 1, "left probe must trigger right avoidance immediately"

call("Avoid_SetDrive", 1)
step(21000, 0x18, 0, 600, 0, 21000)
assert call("Avoid_Mode") == 0
step(21400, 0x18, 0, 280, 0, 21000)
assert call("Avoid_Mode") == 0, "stale near sample must not trigger"
step(21500, 0x18, 0, 280, 0, 21500)
assert call("Avoid_Mode") == 1

# When both probes are near, the closer side chooses the opposite-side arc;
# a difference below 30mm keeps the existing center-default left arc.
for start, l_mm, r_mm, expected in ((22000, 280, 200, "left"),
                                    (23000, 200, 280, "right"),
                                    (24000, 200, 220, "left")):
    call("Avoid_SetDrive", 1)
    step(start, 0x18, 600, 600, start, start)
    before = len(events)
    step(start + 100, 0x18, l_mm, r_mm, start + 100, start + 100)
    assert call("Avoid_Mode") == 1
    expected_event = "A02 right US -> left" if expected == "left" else "A02 left US -> right"
    assert any(expected_event in text for _, text in events[before:]), (l_mm, r_mm, events[before:])
# Cooldown starts at A01 handoff, not at the first black sample.
call("Avoid_SetDrive", 1)
step(25000, 0x18, 0, 600, 0, 25000)
step(25100, 0x18, 0, 280, 0, 25100)
assert call("Avoid_Mode") == 1
step(25600, 0xff, 0, 280, 0, 25100)
step(26030, 0, 0, 280, 0, 25100)
step(26040, 0x18, 0, 280, 0, 25100)
step(26290, 0, 0, 280, 0, 25100)
assert call("Avoid_Mode") == 0, "A02 must hand control back before cooldown"
for time in (26350, 29280, 29290):
    step(time, 0x18, 0, 280, 0, time)
    assert call("Avoid_Mode") == 0, (time, "retriggered during cooldown")
step(29300, 0x18, 0, 280, 0, 29290)
assert call("Avoid_Mode") == 0, "cooldown's last cached sample is not new"
step(29350, 0x18, 0, 280, 0, 29350)
assert call("Avoid_Mode") == 1, "first new sample after cooldown should trigger"

# The 350mm preparation must affect A01's shared base only for this obstacle.
base_addr = call("Tlm_Params") + 12
call("Avoid_SetDrive", 1)
uc.mem_write(base_addr, struct.pack("<i", 45))
step(30000, 0x18, 0, 600, 0, 30000)
assert struct.unpack("<i", uc.mem_read(base_addr, 4))[0] == 45
step(30010, 0x18, 0, 330, 0, 30000)
assert struct.unpack("<i", uc.mem_read(base_addr, 4))[0] == 45, "old cached echo must not slow A01"
step(30100, 0x18, 0, -2, 0, 30100)
assert struct.unpack("<i", uc.mem_read(base_addr, 4))[0] == 45, "negative ultrasonic status is not distance"
step(30200, 0x18, 0, 351, 0, 30200)
assert struct.unpack("<i", uc.mem_read(base_addr, 4))[0] == 45
step(30300, 0x18, 0, 350, 0, 30300)
assert call("Avoid_Mode") == 0
assert struct.unpack("<i", uc.mem_read(base_addr, 4))[0] == 30, "350mm must enter preparation"
step(30400, 0x18, 0, 330, 0, 30400)
assert call("Avoid_Mode") == 0
assert struct.unpack("<i", uc.mem_read(base_addr, 4))[0] == 30
step(30500, 0x18, 0, 300, 0, 30500)
assert call("Avoid_Mode") == 1, "300mm still starts the original avoidance arc"
step(31000, 0xff)
step(31430, 0)
step(31440, 0x18)
step(31690, 0)
assert call("Avoid_Mode") == 0, "line capture must hand off to A01"
assert struct.unpack("<i", uc.mem_read(base_addr, 4))[0] == 45, "handoff must restore cruise base"
step(31700, 0x18, 0, 330, 0, 31700)
assert struct.unpack("<i", uc.mem_read(base_addr, 4))[0] == 45, "cooldown must not re-enter preparation"

call("Avoid_SetDrive", 1)
uc.mem_write(base_addr, struct.pack("<i", 48))
step(32000, 0x18, 0, 330, 0, 32000)
assert struct.unpack("<i", uc.mem_read(base_addr, 4))[0] == 30
call("Avoid_SetDrive", 0)
assert struct.unpack("<i", uc.mem_read(base_addr, 4))[0] == 48, "manual stop must restore the user's cruise setting"

print("PASS: opposite-side direction, mirrored capture/A01 handoff, 4.5s no-line stop, 3s cooldown, 350mm preparation and cruise restoration")
