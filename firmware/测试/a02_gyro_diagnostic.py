"""ARM ELF regression: bad feedback must brake early, never reproduce a half-turn.

Run after building firmware, with unicorn and pyelftools installed:
    python firmware/测试/a02_gyro_diagnostic.py

No hardware access or log writes. Angles/times here refer to the explicit nominal
plant, NOT measured car response or a guaranteed physical stopping distance.
"""
import math
import a02_navigation as nav


def main():
    real_call = nav.call

    cases = [('zero', b) for b in (0, 524, -524)] + [
        (m, 0) for m in ('near_zero', 'opposite', 'io_failure', 'stale')]
    for mode, bias in cases:
        for side in (0, 1):
            configured = False
            def injected_call(name, *args):
                nonlocal configured
                if name == 'Avoid_Step' and not configured:
                    nav.arm.put('s_gz_bias', float(bias))
                    nav.sim['sensor_bias_raw'] = bias
                    configured = True
                if name == 'Avoid_Step' and nav.arm.get('s_phase') not in (0, 7):
                    if mode == 'zero':
                        nav.sim['raw_override'] = 0
                    elif mode == 'near_zero':
                        nav.sim['raw_override'] = round(-nav.sim['rate'] * 131 * .01)
                    elif mode == 'opposite':
                        nav.sim['raw_override'] = round(nav.sim['rate'] * 131)
                    elif mode == 'io_failure':
                        nav.sim.update(gyro_valid=0, gyro_status=2, gyro_failures=1)
                    else:
                        nav.sim['gyro_frozen'] = True
                return real_call(name, *args)
            nav.call = injected_call
            fault = ('AVOID gyro invalid/stale' if mode in ('io_failure', 'stale')
                     else 'AVOID gyro/wheel mismatch')
            try:
                history = nav.run(side, expected_fault=fault)
            finally:
                nav.call = real_call
            start = next(row[0] for row in history if row[1] != 0)
            elapsed = nav.sim['ms'] - start
            yaw = math.degrees(history[-1][4])
            assert elapsed <= (120 if mode in ('io_failure', 'stale') else 600), elapsed
            assert abs(yaw) < 30, yaw
            assert nav.arm.motors[-1] == (0, 0)
            assert 'AVOID footprint guard' not in nav.arm.events
            assert 'AVOID line recovered' not in nav.arm.events
            if mode == 'zero' and not bias:
                assert nav.arm.get('s_yaw_deg', 'f') == nav.arm.get('s_y_mm', 'f') == 0.0
            nav.sim.update(gyro_valid=1, gyro_status=0, gyro_failures=0, gyro_frozen=False)
            nav.sim.pop('raw_override', None)
            for _ in range(30):
                nav.sim['ms'] += 10
                real_call('Avoid_Step')
                assert nav.arm.get('s_phase') == 7 and nav.arm.motors[-1] == (0, 0)
            print('PASS: feedback fault', mode, 'bias-raw', bias, 'side', side,
                  'entry-to-brake-ms', elapsed, 'nominal-yaw-deg', round(yaw, 1), flush=True)

    for condition in ('uncalibrated', 'invalid', 'stale'):
        nav.init(); nav.sim['ms'] = 2000
        nav.arm.put('s_line_ready_ms', 1000)
        if condition == 'uncalibrated':
            nav.arm.put('s_bias_ready', 0)
        elif condition == 'invalid':
            nav.sim['gyro_valid'] = 0
        else:
            nav.sim.update(gyro_stamp=1000, gyro_seq=1, gyro_raw=0, gyro_frozen=True)
        nav.sonar(0, 330); real_call('Avoid_Step')
        nav.sim['ms'] = 2020; nav.sonar(0, 320); real_call('Avoid_Step')
        assert nav.arm.get('s_phase') == 7 and nav.arm.motors[-1] == (0, 0)
        assert 'AVOID gyro not ready' in nav.arm.events
        assert not any(l != r for l, r in nav.arm.motors)
    print('PASS: entry cannot turn on invalid/stale/uncalibrated feedback')

    nav.init(); nav.sim['ms'] = 2000
    nav.arm.put('s_phase', 1); nav.arm.put('s_tick_ms', 1990)
    nav.arm.put('s_start_ms', 1900); nav.arm.put('s_progress_ms', 2000)
    nav.arm.put('s_shift_x', 1000.0); nav.arm.put('s_pass_y', 133.0)
    for tick in range(23):
        nav.sim['ms'] = 2000 + tick*10
        nav.sim['enc'][0] += 10
        nav.sim['rate'] = 0 if tick < 11 else 40
        nav.arm.put('s_pwm_l', 22); nav.arm.put('s_pwm_r', 0)
        real_call('Avoid_Step')
        assert nav.arm.get('s_phase') == 1
    print('PASS: consecutive evidence required; one noisy window tolerated')


if __name__ == '__main__':
    main()
