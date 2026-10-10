"""Actual ARM A02 state machine with a nominal differential-drive plant.

This is NOT hardware clearance certification: footprint, probe mounting, wheel
response and track curves below are explicitly synthetic assumptions. Never
write logs, flash the target, or send car commands. Run after a PlatformIO build.
"""
import math
import struct
import a02_regression as arm
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_CPSR

u, syms = arm.u, arm.symbols
US = 0x2000d000
LIVE = 0x2000d100
sim = {}
extra = {syms[n][0] & ~1: n for n in ['HAL_GetTick', 'En_Count', 'Trace_GetDrive', 'Trace_Online', 'Log_GetGz', 'Log_LiveFrame', 'Us_Get', 'Gr_ReadDigital']}


def hw(uc, address, size, user):
    name = extra.get(address)
    if name is None:
        return
    r0 = uc.reg_read(UC_ARM_REG_R0)
    result = 0
    if name == 'HAL_GetTick':
        result = sim['ms']
    elif name == 'En_Count':
        result = round(sim['enc'][r0]) & 0xffff
    elif name == 'Trace_GetDrive':
        result = sim.get('drive', 1)
    elif name == 'Trace_Online':
        result = 1
    elif name == 'Log_GetGz':
        result = round(-sim['rate'] * 131) & 0xffffffff
    elif name == 'Log_LiveFrame':
        result = LIVE
    elif name == 'Us_Get':
        result = US + r0 * 64
    elif name == 'Gr_ReadDigital':
        uc.mem_write(r0, bytes([sim['bits'] ^ 0xff]))
        result = 0 if sim.get('gray_fail') else 1
    uc.reg_write(UC_ARM_REG_R0, result)
    uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))


for address in extra:
    u.hook_add(arm.UC_HOOK_CODE, hw, begin=address, end=address)


def call(name, *args):
    u.reg_write(UC_ARM_REG_CPSR, 0x20)
    u.reg_write(UC_ARM_REG_SP, 0x2000f000)
    u.reg_write(UC_ARM_REG_LR, arm.STOP | 1)
    for reg, value in zip([UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2], args):
        u.reg_write(reg, value)
    u.emu_start(syms[name][0] | 1, arm.STOP, count=150000)
    assert u.reg_read(UC_ARM_REG_PC) == arm.STOP, name


def init():
    # Clear only A02-owned static variables, not shared A01 or ELF constants.
    for name in ('s_drive', 's_seen', 's_clear_reads', 's_phase', 's_away', 's_obstacle_side',
                 's_prev_mm', 's_us_stamp', 's_good_us_ms', 's_phase_ms', 's_start_ms',
                 's_tick_ms', 's_cool_until', 's_progress_ms', 's_line_ready_ms', 's_center_ms',
                 's_enc_l', 's_enc_r', 's_trip_mm', 's_phase_mm', 's_x_mm', 's_y_mm',
                 's_yaw_deg', 's_clear_x', 's_gz_bias', 's_yaw_rate', 's_steer_out',
                 's_line_err', 's_line_d', 's_line_ms', 's_line_trip', 's_dt_ms',
                 's_pwm_l', 's_pwm_r', 's_speed_ms', 's_speed_dt', 's_search_ms',
                 's_speed_dist', 's_speed_mm_s', 's_speed_i', 's_curve_live', 's_curve_entry',
                 's_offset_mm', 's_shift_x', 's_obstacle_x', 's_obstacle_end', 's_obstacle_y', 's_search_x',
                 's_search_trip', 's_plan_valid', 's_heading_target', 's_pass_y', 's_return_offset'):
        addr, size = syms[name]
        u.mem_write(addr, b'\0' * size)
    sim.clear()
    sim.update(ms=1000, enc=[0.0, 0.0], rate=0.0, bits=0x18)
    u.mem_write(US, b'\0' * 128)
    u.mem_write(LIVE, bytes([0x18, 0, 22, 22, 1, 0, 0, 0]))
    arm.put('s_drive', 1)
    arm.events.clear()
    arm.motors.clear()


def sonar(side, mm, stamp=None):
    # ARM UsDev: mm at byte20, width_us at24, stamp_ms at28, last_ok at32.
    u.mem_write(US + side * 64 + 20, struct.pack('<iII', mm, 0, sim['ms'] if stamp is None else stamp))


def polygon(x, y, angle):
    c, s = math.cos(angle), math.sin(angle)
    return [(x + a*c - b*s, y + a*s + b*c) for a, b in [(-170, -100), (40, -100), (40, 100), (-170, 100)]]


def clearance(car, obstacle):
    def axes(poly):
        for p, q in zip(poly, poly[1:] + poly[:1]):
            yield (-(q[1]-p[1]), q[0]-p[0])
    separated = False
    for a, b in list(axes(car)) + list(axes(obstacle)):
        pc = [x*a+y*b for x,y in car]
        po = [x*a+y*b for x,y in obstacle]
        if max(pc) < min(po) or max(po) < min(pc):
            separated = True
    if not separated:
        return 0.0
    def point_seg(p, a, b):
        vx, vy = b[0]-a[0], b[1]-a[1]
        t = max(0, min(1, ((p[0]-a[0])*vx + (p[1]-a[1])*vy)/(vx*vx+vy*vy)))
        return math.hypot(p[0]-a[0]-t*vx, p[1]-a[1]-t*vy)
    return min(point_seg(p,a,b) for pts, seg in [(car,obstacle), (obstacle,car)] for p in pts for a,b in zip(seg,seg[1:]+seg[:1]))


def run(side, bend=0.0, no_line=False, motor_gain=7.5, unseen_bend=False,
        expected_fault=None, stale_curve=0.0, motor_tau=0.0, gyro_period=1):
    init()
    x, y, yaw = -120.0, 0.0, 0.0
    if bend and not unseen_bend:
        y, yaw = .5*bend*x*x, math.atan(bend*x)
    front = 400.0
    near = .5*bend*front*front if not unseen_bend else 0.0
    oy0, oy1 = (near-150, near) if side == 0 else (near, near+150)
    obstacle = [(front,oy0), (front+150,oy0), (front+150,oy1), (front,oy1)]
    entered = False
    wheel_speed = [0.0, 0.0]
    sensed_rate = 0.0
    minimum = float('inf')
    history = []
    for tick in range(3500):
        sim['ms'] = 1000 + tick*10
        phase = arm.get('s_phase')
        if phase == 0 and entered:
            break
        if phase == 0:
            # Outside A02 scope, A01 supplies stable normal-trace commands.
            arm.motors.append((22, 22))
        bits = 0
        for k in range(8):
            lateral = (k-3.5)*8
            sx = x + 40*math.cos(yaw) - lateral*math.sin(yaw)
            sy = y + 40*math.sin(yaw) + lateral*math.cos(yaw)
            line = .5*bend*(max(0,sx-300)**2 if unseen_bend else sx*sx)
            if abs(sy-line) <= 10:
                bits |= 1 << k
        if no_line and entered:
            bits = 0
        sim['bits'] = bits
        if phase == 0:
            u.mem_write(LIVE, bytes([bits, 0, 22, 22, 1, 0, 0, 0]))
            if stale_curve:
                # Regression fixture: tight bend just ended, actual rate is zero
                # but the v3 EMA still contains the bend (15:44-3 pattern).
                arm.put('s_curve_live', stale_curve)
        if tick % 20 == 0:
            for which in (0,1):
                # Once outside the beam, -2 must not be treated as clearance proof.
                mm = round((front-x-40)/.9659) if which == side and phase == 0 else -2
                sonar(which, mm)
        sim['rate'] = sensed_rate
        call('Avoid_Step')
        phase = arm.get('s_phase')
        if phase != 0:
            if not entered and stale_curve:
                assert arm.get('s_curve_entry', 'f') == 0.0
                assert abs(arm.get('s_pass_y', 'f')) >= 133.0
            entered = True
        if phase == 7:
            break
        l, r = arm.motors[-1]
        assert 0 <= l <= 36 and 0 <= r <= 36, (l,r)
        vl, vr = max(0,l-4)*motor_gain, max(0,r-4)*motor_gain
        if motor_tau:
            alpha = 1-math.exp(-.01/motor_tau)
            wheel_speed[0] += alpha*(vl-wheel_speed[0])
            wheel_speed[1] += alpha*(vr-wheel_speed[1])
            vl, vr = wheel_speed
        v, rate = (vl+vr)/2, (vl-vr)/150
        if phase == 0 and not entered and bend and not unseen_bend:
            # Perfect A01 tracking is outside this test's scope; expose a real
            # pre-entry curve through encoders and gyro, not a fabricated FSM hint.
            rate = v*bend / (1+(bend*x)**2)**1.5
            vl, vr = v + rate*75, v - rate*75
        if tick % gyro_period == 0:
            sensed_rate = math.degrees(rate)
        sim['enc'][0] += vl*.01/.115
        sim['enc'][1] += vr*.01/.115
        mid = yaw + rate*.005
        x += v*.01*math.cos(mid)
        y += v*.01*math.sin(mid)
        yaw += rate*.01
        minimum = min(minimum, clearance(polygon(x,y,yaw), obstacle))
        history.append((sim['ms'], phase, x, y, yaw, v))
    result = (arm.get('s_phase'), round(minimum,1), arm.events[:])
    tangent = math.atan(bend*(max(0,x-300) if unseen_bend else x))
    heading_error = abs(math.degrees(yaw-tangent))
    print('MODEL', side, bend, no_line, motor_gain, 'unseen', unseen_bend,
          'stale', stale_curve, 'tau', motor_tau, 'gyro-ticks', gyro_period,
          'handoff-angle', round(heading_error,1), result, flush=True)
    if no_line or expected_fault:
        assert result[0] == 7 and (expected_fault or 'AVOID line not found') in result[2], result
        assert arm.motors[-1] == (0,0)
    else:
        assert entered and result[0] == 0 and 'AVOID line recovered' in result[2], result
        assert minimum >= 20.0, minimum
        assert heading_error <= 18.0, heading_error
    return history


if __name__ == '__main__':
    for side in (0,1):
        for bend in (0.0, -.0005, .0005, -.001, .001):
            run(side, bend)
    for gain in (6.0,9.0):
        run(1, motor_gain=gain)
    for side, curve in ((0,-.00114),(1,.00114)):
        run(side, stale_curve=curve)
    for side in (0,1):
        run(side, motor_tau=.05, gyro_period=4)
    run(1, no_line=True)
    # A bend beginning only after all line sensing is lost is unobservable.
    # Keep this failed-recovery case: it must brake, not masquerade as success.
    run(0, -.0005, unseen_bend=True, expected_fault='AVOID line not found')
    # Real firmware safety paths, independent of plant trajectory.
    init(); sim['ms']=2000; arm.put('s_phase',5); arm.put('s_start_ms',1900)
    arm.put('s_tick_ms',1990); arm.put('s_progress_ms',2000)
    arm.put('s_search_ms',1900); sim['gray_fail']=True
    call('Avoid_Step')
    assert arm.get('s_phase') == 7 and 'AVOID gray read failed' in arm.events
    init(); sim['ms']=2000; sonar(0,25); call('Avoid_Step')
    assert arm.get('s_phase') == 7 and 'AVOID emergency <=30mm' in arm.events
    init(); sim['ms']=2000; arm.put('s_line_ready_ms',1000); sonar(0,330)
    call('Avoid_Step'); assert arm.get('s_phase') == 0
    sim['ms']=2010; call('Avoid_Step')
    assert arm.get('s_phase') == 0
    sonar(0,320); sim['ms']=2020; call('Avoid_Step')
    assert arm.get('s_phase') == 1, arm.events
    init(); sim['ms']=2000; arm.put('s_line_ready_ms',1000); sonar(0,200)
    call('Avoid_Step'); sim['ms']=2020; sonar(0,190); call('Avoid_Step')
    assert arm.get('s_phase') == 7 and 'AVOID insufficient turn room' in arm.events
    init(); sim['ms']=2000; arm.put('s_line_ready_ms',1000); sonar(0,25,1000)
    call('Avoid_Step'); assert arm.get('s_phase') == 0  # stale emergency is not a fresh detection
    leads=[]
    for speed in (0.0,100.0,200.0,600.0):
        arm.put('s_speed_mm_s',speed); call('av_trigger_mm')
        leads.append(struct.unpack('<f',struct.pack('<I',u.reg_read(UC_ARM_REG_R0)))[0])
    assert leads[0] < leads[1] < leads[2] < leads[3] == 500.0, leads
    init(); sim['ms']=2000; arm.put('s_phase',4); arm.put('s_start_ms',1900)
    arm.put('s_tick_ms',1990); arm.put('s_progress_ms',2000); arm.put('s_plan_valid',1)
    arm.put('s_obstacle_end',150.0); arm.put('s_clear_x',500.0)
    call('Avoid_Step')
    assert arm.get('s_phase') == 7 and 'AVOID footprint guard' in arm.events
    sim['ms']=2010; call('Avoid_Step')
    assert arm.get('s_phase') == 7 and arm.motors[-1] == (0,0)
    init(); sim['ms']=2000; arm.put('s_phase',4); arm.put('s_start_ms',1900)
    arm.put('s_tick_ms',1900); arm.put('s_progress_ms',2000)
    call('Avoid_Step')
    assert arm.get('s_phase') == 7 and 'AVOID control delayed' in arm.events
    # Curvature rejection and initial slew are control decisions, not clearance proof.
    for side in (0,1):
        for old_curve in (-.0014,.0014):
            init(); sim['ms']=2000; arm.put('s_line_ready_ms',1000)
            arm.put('s_curve_live',old_curve); arm.put('s_speed_mm_s',117.0)
            sonar(side,370); call('Avoid_Step')
            sim['ms']=2020; sonar(side,367); call('Avoid_Step')
            assert arm.get('s_phase') == 1 and arm.get('s_curve_entry','f') == 0.0
            away = 1 if side == 0 else -1
            assert abs(arm.get('s_pass_y','f') - away*133.0) < .01
            l,r=arm.motors[-1]
            assert (l-r)*away > 0 and abs(l-r) <= 2, (side,l,r)
    print('PASS: tight-bend residual rejected, mirrored entry slew, delayed gyro and inertial wheels')
    print('PASS: nominal straight/curved recovery, drive gain variation, bounded white search, gray failure and emergency brake')
    print('PASS: fresh double confirmation, stale rejection, insufficient room, speed-related lead, footprint/fault latch and scheduling guard')
