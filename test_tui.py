#!/usr/bin/env python3
"""Socket framing, stale display, reconnect, and terminal cleanup checks."""
import importlib.util
import json
import os
from pathlib import Path
import pty
import select
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('watch', ROOT / 'client/watch.py')
watch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(watch)
listener = socket.socket()
listener.bind(('127.0.0.1', 0))
listener.listen()
s = watch.Stream(*listener.getsockname())
pose = {'pose': [1, 0, 0, 1, 0, 1, 0, 2, 0, 0, 1, 3, 0, 0, 0, 1],
        'position_tracked': True, 'orientation_tracked': True}
sample = {'schema': 'teleop.quest_state.v1', 'session_id': '1', 'frame_seq': 1,
          'head': pose, 'hands': {'left': pose, 'right': pose}, 'input_focused': False}
sample['control_intent'] = {'hands': {'left': {'valid': True, 'engaged': True,
    'needs_release': False, 'engagement_id': 7, 'delta_position': [0.1, 0, 0]}}}
sample['demo'] = {'enabled': True, 'base_frame': 'live'}
wire = json.dumps(sample).encode() + b'\n'
s.tick()
peer, _ = listener.accept()
peer.sendall(wire[:30])
s.tick()
assert s.sample is None
peer.sendall(wire[30:] + b'bad json\n' + wire)
s.tick()
assert s.sample == sample and s.bad == 1
assert any('SYSTEM / TERMINAL' in text for text, _ in watch.view(s))
assert any('LEFT clutch ENGAGED #7' in text for text, _ in watch.view(s))
assert any('LOCAL KINEMATIC DEMO' in text for text, _ in watch.view(s))
assert '+1.000'  in watch.pose_lines(pose)[0]
s.received -= 2
assert any('STALE' in text for text, _ in watch.view(s))
peer.close()
s.tick()
assert s.sample is None
s.retry = 0
s.tick()
peer, _ = listener.accept()
sample['session_id'] = '2'
peer.sendall(json.dumps(sample).encode() + b'\n')
s.tick()
assert s.sample['session_id'] == '2'
s.close()
peer.close()
# Exercise actual curses startup/quit in a PTY, rather than only rendering helpers.
master, slave = pty.openpty()
proc = subprocess.Popen([sys.executable, str(ROOT / 'client/watch.py'), '--port',
                         str(listener.getsockname()[1])], stdin=slave, stdout=slave,
                        stderr=slave, env=dict(os.environ, TERM='xterm-256color'))
os.close(slave)
output = b''
deadline = time.monotonic() + 5
try:
    while b'QUEST TELEMETRY' not in output and time.monotonic() < deadline:
        if select.select([master], [], [], 0.1)[0]:
            output += os.read(master, 65536)
    assert b'QUEST TELEMETRY' in output, output
    os.write(master, b'q')
    assert proc.wait(timeout=3) == 0
finally:
    if proc.poll() is None:
        proc.kill()
        proc.wait()
    os.close(master)
    listener.close()
print('PASS: fragmented records, invalid JSON, stale/focus display, disconnect, reconnect, curses quit')
