"""ARM integration: actual A01 -> A02 recovery -> actual A01, no A01 source edits.

Hardware is mocked; verifies arbitration and command continuity only, not real
track-following quality. No hardware access or logs. Run after PlatformIO build.
"""
import struct
import a02_navigation as nav
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_PC, UC_ARM_REG_LR

u, syms, arm = nav.u, nav.syms, nav.arm
PARAM = 0x2000da00
names = ['Gr_Ping', 'Tlm_Params', 'printf', 'puts', 'Bsp_FmtFix', 'Mo_Stop']
hooks = {syms[n][0] & ~1: n for n in names if n in syms}
trace_locals = {}
with arm.elf_path.open('rb') as f:
    owner = ''
    for sym in arm.ELFFile(f).get_section_by_name('.symtab').iter_symbols():
        if sym['st_info']['type'] == 'STT_FILE':
            owner = sym.name
        elif owner == 'app_trace.c':
            trace_locals[sym.name] = (sym['st_value'], sym['st_size'])


def trace_read(name):
    address, size = trace_locals[name]
    return bytes(u.mem_read(address, size))
# Real public A01 drive/online functions must run, not navigation fixtures.
nav.extra.pop(syms['Trace_GetDrive'][0] & ~1)
nav.extra.pop(syms['Trace_Online'][0] & ~1)
arm.hooks.pop(syms['Trace_FinReset'][0] & ~1)


def extra(uc, address, size, user):
    name = hooks.get(address)
    if name is None:
        return
    result = 0
    if name == 'Gr_Ping':
        result = 1
    elif name == 'Tlm_Params':
        result = PARAM
    elif name == 'Mo_Stop':
        arm.motors.append((0, 0))
    elif name == 'Bsp_FmtFix':
        uc.mem_write(uc.reg_read(UC_ARM_REG_R0), b'0\0')
    uc.reg_write(UC_ARM_REG_R0, result)
    uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))


for addr in hooks:
    u.hook_add(arm.UC_HOOK_CODE, extra, begin=addr, end=addr)


def main():
    for bits in (0x18, 0x0c, 0x30):
        nav.init()
        u.mem_write(PARAM, struct.pack('<fffi', 21, .05, .68, 22))
        nav.call('Trace_Init')
        nav.call('Trace_StartRun')
        nav.call('Trace_SetDrive', 1)
        # Exercise real A01, including mild steering retained across suspension.
        for tick in range(40):
            nav.sim.update(ms=1000+tick*10, bits=bits if tick > 30 else 0x18)
            nav.call('Trace_Step', 1)
        assert arm.motors[-1][0] > 0 or arm.motors[-1][1] > 0
        # Keep a nontrivial finish-arming/odometry snapshot across the handoff.
        u.mem_write(trace_locals['s_fin_armed'][0], b'\x01')
        u.mem_write(trace_locals['s_odo_cnt'][0], struct.pack('<i', 9000))
        finish = {n: trace_read(n) for n in ('s_fin_armed', 's_odo_cnt', 's_fin_mm')}
        # A02 RECOVER operates on the observed local line, with main's A01 pause.
        arm.put('s_phase', 6); arm.put('s_phase_ms', nav.sim['ms'])
        arm.put('s_tick_ms', nav.sim['ms']); arm.put('s_start_ms', nav.sim['ms'])
        arm.put('s_progress_ms', nav.sim['ms']); arm.put('s_plan_valid', 0)
        arm.put('s_line_ms', 0); arm.put('s_center_ms', 0)
        arm.put('s_speed_mm_s', 50.0); arm.put('s_steer_out', 0.0)
        for tick in range(1, 40):
            nav.sim.update(ms=1390+tick*10, bits=0x18, rate=0.0)
            nav.sim['enc'][0] += 5; nav.sim['enc'][1] += 5
            nav.call('Avoid_Step')
            if arm.get('s_phase') == 0:
                break
        assert arm.get('s_phase') == 0 and 'AVOID line recovered' in arm.events
        assert finish == {n: trace_read(n) for n in finish}, 'release must not re-arm/reset race odometry'
        before = arm.motors[-1]
        # Exactly main.c's next iteration: Trace_Step(1) then Avoid_Step().
        ramp = []
        for _ in range(30):
            nav.sim['ms'] += 10
            nav.call('Trace_Step', 1)
            nav.call('Avoid_Step')
            assert arm.get('s_phase') == 0
            l, r = arm.motors[-1]
            assert 0 < l <= 22 and 0 < r <= 22, (bits, l, r)
            assert abs(l-r) <= 8, 'stale A01 derivative/steer causes harsh handoff'
            ramp.append((l, r))
        assert ramp[0] == (1, 1), 'handoff must use existing low-PWM A01 ramp'
        assert arm.motors[-1][0] == arm.motors[-1][1]
        assert not any('FIN' in e or 'AVOID gyro' in e for e in arm.events)
        print('PASS: actual A01 resumes after A02 release, prior gray', hex(bits),
              'handoff PWM', before, 'settled A01 PWM', arm.motors[-1])


if __name__ == '__main__':
    main()
