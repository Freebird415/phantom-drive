"""Read-only original BLE evidence audit. Never rewrite CSV/derived data.

gz is already deg/s. Use same-kind E timestamps for takeover-to-fault durations;
do not sum sampled 10ms encoder increments as full-trip distance or yaw truth.
"""
import collections
import csv
import hashlib
from pathlib import Path
import sys

sys.stdout.reconfigure(encoding='utf-8')
ROOT = Path(__file__).resolve().parents[2]
HASHES = {
    '2026-10-10_1607-2/telemetry.csv': '688c10824fd676ad78ae48ff8fc6ef9bb9a2825c3c14620565f54393e0342225',
    '2026-10-10_1607-2/meta.json': '29dbe33640a99988f4476016f62ba8c1c9341bc7576379a3b46616ffff84f277',
    '2026-10-10_1607-2/note.md': '7324a41947a216e836965e5e5e582c771c5ca19bccd1b9cf8652c58df2a37c96',
    '2026-10-10_1608/telemetry.csv': 'cec3c00ce5c84247a1d75409f605d834950b6184bf930d24100ec051c6bfd738',
    '2026-10-10_1608/meta.json': '886bb591fb3a7fe551d19c27ee1e9f6f0f346e105fd0843c7f44fb5a98302b21',
    '2026-10-10_1608/note.md': '788440c7786fda0d492a6389f978fdaf567c7e1db0cdeae67aface72dd0c8e14',
    '2026-10-10_1608/片段_第五次测试/telemetry.csv': '69278555b4be55b1a53e4883401ace2d7c34005f4bdd330c286ce1969711a25c',
    '2026-10-10_1608/片段_第五次测试/derived.csv': 'b65f41a61fe82c621161bc0b599ccd11da004e9b1e13dcf779ba5f109c959f66',
    '2026-10-10_1608/片段_第五次测试/meta.json': '04be57c12b0c5781f58c71c2c6385fdbc5991a44aac797732ecf328c1b42431c',
    '2026-10-10_1608/片段_第五次测试/note.md': '8154b2e525bfe706c0d34144c416374221b0ec429a11d62b962fe32b48ae129d',
}


def main():
    for name, expected in HASHES.items():
        assert hashlib.sha256((ROOT/'logs'/name).read_bytes()).hexdigest() == expected, name
    for name, nt, nu, ne, strong, dt in (
        ('2026-10-10_1607-2', 522, 42, 7, 149, 2741),
        ('2026-10-10_1608', 792, 65, 7, 153, 2864),
        ('2026-10-10_1608/片段_第五次测试', 787, 64, 7, 153, 2864),
    ):
        with (ROOT/'logs'/name/'telemetry.csv').open(encoding='utf-8-sig', newline='') as f:
            rows = list(csv.DictReader(f))
        counts = collections.Counter(r['kind'] for r in rows)
        assert (counts['T'], counts['U'], counts['E']) == (nt, nu, ne)
        ts = [r for r in rows if r['kind'] == 'T']
        assert all(r['gz'] != '' and float(r['gz']) == 0 for r in ts)
        assert not any(r['state'] == '3' for r in ts)
        contrast = sum(abs(float(r['pwm_l'])-float(r['pwm_r'])) >= 10 and
                       abs(float(r['dl'])-float(r['dr'])) >= 3 for r in ts)
        assert contrast == strong
        es = [r for r in rows if r['kind'] == 'E']
        near = next(r for r in es if ' near ' in r['note'])
        stop = next(r for r in es if r['note'] == 'AVOID footprint guard')
        assert int(stop['ms'])-int(near['ms']) == dt
        assert not any('line recovered' in r['note'] or 'return now' in r['note'] for r in es)
        print('PASS: original evidence', name, f'T/U/E={nt}/{nu}/{ne}',
              f'explicit-zero={nt}, missing=0, contrast={contrast}, E-dt={dt}ms')
    print('PASS: all 10 original telemetry/meta/note/derived files unchanged by SHA-256')


if __name__ == '__main__':
    main()
