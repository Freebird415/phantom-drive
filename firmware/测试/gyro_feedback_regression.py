"""Execute real MPU driver -> log cache -> A02 ARM code with fake I2C registers.

Offline only, no hardware/log writes. Tests failures, cache ages, initialization,
parked retry, raw-vs-gz8 quantization, and calibration on distinct accepted reads.
"""
import struct
import a02_regression as arm
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                              UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR,
                              UC_ARM_REG_PC, UC_ARM_REG_CPSR)

u, syms = arm.u, arm.symbols
OUT = 0x2000d800
US = 0x2000d900
regs = {0x75: 0x68, 0x6b: 0x40, 0x1a: 0, 0x1b: 0, 0x1c: 0}
state = dict(drive=0, raw=[0, 0, 16384, 0, 0, 0, 0], read_fail=None,
             write_fail=None, bus_fail=False, bad_back=False)
transactions = []
ble = []
for name in ('Log_Note', 'Log_GetGyro'):
    arm.hooks.pop(syms[name][0] & ~1)

names = ['HAL_I2C_Init', 'HAL_I2C_Mem_Read', 'HAL_I2C_Mem_Write',
         'Trace_GetDrive', 'En_Count', 'Us_Get', 'Gr_ReadDigital', 'Bsp_Uart2Tx', 'printf', 'puts']
hooks = {syms[n][0] & ~1: n for n in names if n in syms}


def mock(uc, address, size, user):
    name = hooks.get(address)
    if not name:
        return
    a = [uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
    result = 0
    if name == 'HAL_I2C_Init':
        result = 1 if state['bus_fail'] else 0
    elif name in ('HAL_I2C_Mem_Read', 'HAL_I2C_Mem_Write'):
        ptr, count, timeout = struct.unpack('<III', bytes(uc.mem_read(uc.reg_read(UC_ARM_REG_SP), 12)))
        reg = a[2]
        transactions.append((arm.clock, name, reg, count, timeout, state['drive']))
        assert a[1] == 0xd0 and timeout == 5
        fail = state['read_fail'] if name.endswith('Read') else state['write_fail']
        if fail in ('all', reg):
            result = 1
        elif name.endswith('Read'):
            data = struct.pack('>7h', *state['raw']) if reg == 0x3b else bytes([regs.get(reg, 0)])
            if state['bad_back'] and reg == 0x1a:
                data = b'\x00'
            uc.mem_write(ptr, data[:count])
        else:
            regs[reg] = uc.mem_read(ptr, 1)[0]
    elif name == 'Trace_GetDrive':
        result = state['drive']
    elif name == 'Us_Get':
        result = US + a[0]*64
    elif name == 'Gr_ReadDigital':
        uc.mem_write(a[0], b'\xe7'); result = 1
    elif name == 'Bsp_Uart2Tx':
        assert a[1] <= 71, 'event byte count must fit actual stack buffer'
        ble.append(bytes(uc.mem_read(a[0], a[1])))
    uc.reg_write(UC_ARM_REG_R0, result)
    uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))


for address in hooks:
    u.hook_add(arm.UC_HOOK_CODE, mock, begin=address, end=address)


def call(name, *args):
    u.reg_write(UC_ARM_REG_CPSR, 0x20)
    u.reg_write(UC_ARM_REG_SP, 0x2000f000)
    u.reg_write(UC_ARM_REG_LR, arm.STOP | 1)
    for reg, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
        u.reg_write(reg, value)
    for i, value in enumerate(args[4:]):
        u.mem_write(0x2000f000+i*4, struct.pack('<I', value))
    u.emu_start(syms[name][0] | 1, arm.STOP, count=200000)
    assert u.reg_read(UC_ARM_REG_PC) == arm.STOP, name
    return u.reg_read(UC_ARM_REG_R0)


def sample(ms):
    arm.clock = ms
    call('Log_Note', 0x18, 0, 0, 0, 0)  # err float=0 passed in integer ABI register
    call('Log_GetGyro', OUT)
    return struct.unpack('<IIhHBBxx', bytes(u.mem_read(OUT, 16)))


def main():
    # Init cannot silently accept write/readback failures, sleep, wrong WHO/range.
    arm.clock = 1000
    for mode in ('bus', 'who', 'write', 'readback', 'wrong_value'):
        state.update(bus_fail=mode == 'bus', write_fail=0x6b if mode == 'write' else None,
                     read_fail=0x6b if mode == 'readback' else None, bad_back=mode == 'wrong_value')
        regs[0x75] = 0x70 if mode == 'who' else 0x68
        call('Mpu_Init')
        assert call('Mpu_Status') != 0 and call('Mpu_Read', OUT) == 0, mode
    state.update(bus_fail=False, write_fail=None, read_fail=None, bad_back=False)
    regs[0x75] = 0x68
    call('Mpu_Init'); assert call('Mpu_Status') == 0
    print('PASS: strict init, identity, writes and all readbacks')

    # Real zero Z rate + nonzero gravity is valid; raw 131 -> gz8=0, not a failed sensor.
    state['raw'][6] = 131
    g = sample(1040)
    assert g[2] == 131 and g[4:] == (1, 0)
    live = call('Log_LiveFrame')
    assert u.mem_read(live+7, 1)[0] == 0 and call('Log_GetGz') == 131
    old = g
    assert sample(1050) == old, 'same cached sample must not gain a new timestamp/seq'
    print('PASS: actual raw /131 and gz8 /2 quantization, distinct zero vs failure')

    state['drive'] = 1; state['read_fail'] = 0x3b
    writes = sum(t[1].endswith('Write') for t in transactions)
    # Cache still valid on entry; this cycle's real Log_Note discovers IO loss.
    arm.clock = 1080
    arm.put('s_drive', 1); arm.put('s_phase', 1); arm.put('s_tick_ms', 1070)
    arm.put('s_start_ms', 1080); arm.put('s_progress_ms', 1080)
    arm.put('s_shift_x', 1000.0); arm.put('s_pass_y', 133.0)
    call('Avoid_Step')
    assert arm.get('s_phase') == 7 and arm.motors[-1] == (0, 0)
    print('PASS: real MPU failure discovered in current Log_Note brakes in SAME A02 step')
    for ms in range(1080, 2081, 40):
        g = sample(ms)
        assert g[0:3] == old[0:3] and not g[4] and g[5] == 2
    assert g[3] == 26
    assert sum(t[1].endswith('Write') for t in transactions) == writes, 'no moving reinit'
    # A02 consumes the real invalid cache and brakes before any navigation output.
    arm.put('s_drive', 1); arm.put('s_phase', 1); arm.put('s_tick_ms', arm.clock-10)
    arm.put('s_start_ms', arm.clock); arm.put('s_progress_ms', arm.clock)
    call('Avoid_Step')
    assert arm.get('s_phase') == 7 and arm.motors[-1] == (0, 0)
    assert 'AVOID gyro invalid/stale' in arm.events
    print('PASS: 26 consecutive real MPU read failures -> invalid old cache -> latched A02 brake')

    state['drive'] = 0; state['read_fail'] = None
    before = arm.get('s_reinit_cnt')
    sample(2120)
    assert arm.get('s_reinit_cnt') == before+1
    call('Log_GetGyro', OUT)
    good = struct.unpack('<IIhHBBxx', bytes(u.mem_read(OUT, 16)))
    assert good[4:] == (1, 0) and good[3] == 0 and good[1] == old[1]+1
    print('PASS: parked-only rate-limited recovery, fresh cache after success')

    # Sleep with retained nonzero data, altered full scale, six zero axes.
    state['drive'] = 1
    for reg, value, status in ((0x6b, 0x40, 5), (0x1b, 0x18, 3)):
        regs[reg] = value
        assert call('Mpu_Read', OUT) == 0 and call('Mpu_Status') == status
        regs[reg] = 0
    state['raw'] = [0]*7
    assert call('Mpu_Read', OUT) == 0 and call('Mpu_Status') == 4
    state['raw'][2] = 16384
    assert call('Mpu_Read', OUT) == 1 and call('Mpu_Status') == 0
    print('PASS: sleep/frozen registers, scale drift, six-zero rejection; true stationary zero accepted')

    # Stale metadata is rejected even when last read succeeded.
    state['raw'][6] = 0
    sample(2160); state['drive'] = 1
    arm.clock = 2250
    arm.put('s_drive', 1); arm.put('s_phase', 1); arm.put('s_tick_ms', 2240)
    call('Avoid_Step')
    assert arm.get('s_phase') == 7 and arm.motors[-1] == (0, 0)
    print('PASS: 90ms old real cache prevents navigation')

    # Calibration counts fresh samples, not repeated 100Hz uses of one sample.
    state['drive'] = 0
    call('Avoid_Init')
    for ms in range(2300, 2941, 10):
        sample(ms); call('Avoid_Step')
        if ms < 2900:
            assert arm.get('s_bias_ready') == 0
    assert arm.get('s_bias_ready') == 1 and arm.get('s_bias_n') == 16
    assert not any(t[1].endswith('Write') and t[5] for t in transactions)
    print('PASS: parked calibration requires 16 distinct good readings, not repeated cache')

    # Long existing E notes cannot leak beyond the snprintf stack buffer.
    u.mem_write(OUT, b'x'*160+b'\0')
    call('Log_BleEvent', 3, OUT)
    assert len(ble[-1]) == 71 and ble[-1].endswith(b'\r\n')
    print('PASS: BLE event truncation retains framing and bounds byte count')


if __name__ == '__main__':
    main()
