#!/usr/bin/env python3
"""Body tracking host test: native/body_tracking.h's configuration, joint subsets and
JSON (BODY_TRACKING.md), and the golden fixtures/body_frames.jsonl (robot_utils'
reader tests use a copy). `--update` rewrites the fixture."""
from pathlib import Path
import json
import math
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent
FIXTURE = ROOT / 'fixtures/body_frames.jsonl'

with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'test_body_tracking'
    subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'native'),
                    str(ROOT / 'test_body_tracking.cpp'), '-o', str(exe)], check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
records = {r['case']: r for r in map(json.loads, out.splitlines())}

FINGERS = set(range(20, 44)) | set(range(46, 70))
for name, count, set_name, all_joints in (('default_core', 70, 'default', False), ('default_all', 70, 'default', True),
                                          ('full_body_core', 84, 'full_body', False)):
    b = records[name]['body']
    assert b['schema'] == 'teleop.quest_body.v1' and b['available'] and b['active'], b
    assert b['joint_set'] == set_name and b['joint_count'] == count
    assert b['joint_set_id'] == (0 if set_name == 'default' else 1000274000)
    ids = [i for i in range(count) if all_joints or i not in FINGERS]
    assert b['joints'] == ids, b['joints']
    assert len(b['flags']) == len(ids) and len(b['poses']) == 7 * len(ids)
    assert b['subset'] == ('all' if all_joints else 'core')
    assert b['confidence'] == 0.875 and b['fidelity'] == 'high' and b['fidelity_requested'] == 'high'
    assert b['skeleton_changed_count'] == 3 and b['time_ns'] == 123456789012345
    for k, i in enumerate(ids):
        assert b['flags'][k] == (3 if i % 5 == 4 else 15)
        x, y, z, qx, qy, qz, qw = b['poses'][7 * k:7 * k + 7]
        assert abs(x - 0.01 * i) < 1e-4 and abs(y - (1.5 - 0.02 * i)) < 1e-4 and abs(z + 0.001 * i) < 1e-4
        assert abs(qy - math.sin(0.005 * i)) < 1e-4 and abs(qw - math.cos(0.005 * i)) < 1e-4 and qx == qz == 0
b = records['inactive']['body']
assert b['available'] and not b['active'] and b['joints'] == b['flags'] == b['poses'] == [] and b['fidelity'] is None
b = records['nonfinite']['body']
k = b['joints'].index(11)
assert b['flags'][k] == 0 and b['poses'][7 * k] == 0, b['poses'][7 * k:7 * k + 7]
k = b['joints'].index(16)
assert '-0.0000' not in json.dumps(b) and b['fidelity'] is None and b['fidelity_requested'] is None
b = records['unavailable']['body']
assert b == {'schema': 'teleop.quest_body.v1', 'available': False,
             'reason': 'xrCreateBodyTrackerFB failed (-1000710001); BODY_TRACKING permission "not granted"'}, b
size = len(json.dumps(records['default_core']['body'], separators=(',', ':')))
size_all = len(json.dumps(records['default_all']['body'], separators=(',', ':')))
if '--update' in sys.argv:
    FIXTURE.write_text(out)
assert FIXTURE.read_text() == out, 'fixtures/body_frames.jsonl differs from the native output (run with --update)'
print(f'PASS: body tracking config, subsets, JSON, fixture (core block {size} B, all joints {size_all} B)')
