"""Reproduce the KNOWN, UNFIXED v4 zero-gyro failure using actual ARM ELF.

Run after building firmware, with unicorn and pyelftools installed:
    python firmware/tests/a02_gyro_diagnostic.py

No hardware access or log writes. The plant is synthetic, not a measurement
of physical yaw or clearance. A passing diagnostic means the BUG reproduced,
not that feedback-loss handling is safe. Control regressions run on import first.
"""
import math
import a02_navigation as nav


def main():
    real_call = nav.call

    def stuck_gyro_call(name, *args):
        if name == 'Avoid_Step':
            nav.sim['rate'] = 0.0
        return real_call(name, *args)

    nav.call = stuck_gyro_call
    try:
        for side in (0, 1):
            history = nav.run(side, expected_fault='AVOID footprint guard')
            plant_yaw = math.degrees(history[-1][4])
            firmware_yaw = nav.arm.get('s_yaw_deg', 'f')
            firmware_y = nav.arm.get('s_y_mm', 'f')
            motor = nav.arm.motors[-1]
            print('KNOWN BUG REPRODUCED: side', side,
                  'nominal-plant-yaw-deg', round(plant_yaw, 1),
                  'firmware-yaw-deg', firmware_yaw,
                  'firmware-y-mm', firmware_y, 'last-motor', motor)
            assert firmware_yaw == firmware_y == 0.0
            assert abs(plant_yaw) > 170.0, 'Half-turn failure did not reproduce'
            assert motor == (0, 0)
    finally:
        nav.call = real_call


if __name__ == '__main__':
    main()
