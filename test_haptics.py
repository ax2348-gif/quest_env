#!/usr/bin/env python3
"""Haptics host test: native/haptics.h's parsing and scheduling (scripted Player cases) and its stream client
against a little chunked-NDJSON server standing in for robot_utils' GET /haptics?stream=1 (HAPTICS.md): each
pulse played once across a reconnect, a level applied and stopped at its TTL, the ack block."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import json
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parent
SCHEMA = 'teleop.quest_haptics.v1'


def apply(c, hand, t=None, amp=None, dur=None, id_=None):
    return (c['hand'] == hand and c['action'] == 'apply' and (t is None or c['t_ms'] == t)
            and (amp is None or abs(c['amplitude'] - amp) < 1e-6) and (dur is None or c['duration_ms'] == dur)
            and (id_ is None or c['id'] == id_))


with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'test_haptics'
    subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread', '-I', str(ROOT / 'native'),
                    str(ROOT / 'test_haptics.cpp'), '-o', str(exe)], check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
    r = {x['case']: x for x in map(json.loads, out.splitlines())}

    # -- scripted ------------------------------------------------------------------------------------------------
    a = r['armed']
    assert a['ok'] and len(a['first']) == 2, a
    assert apply(a['first'][0], 'left', 0, 0.6, 60, 1) and apply(a['first'][1], 'right', 0, 0.6, 60, 2), a
    assert a['again'] == [], 'a pulse id plays once'
    assert a['block'] == {'schema': SCHEMA, 'epoch': 'e1', 'last_id': 2, 'connected': False}, a['block']
    t = r['two_and_replace']
    assert [c['t_ms'] for c in t['two']] == [0, 140] and [c['id'] for c in t['two']] == [1, 2], t['two']
    assert [c['id'] for c in t['replaced_first']] == [1]
    assert [(c['t_ms'], c['id'], c['duration_ms']) for c in t['replaced_then']] == [(20, 3, 400)], \
        'a newer group drops the waiting second pulse'
    assert r['late'] == {'case': 'late', 'cmds': [], 'last_id': 5, 'skipped': 1}, r['late']
    lv = r['level']
    assert [c['t_ms'] for c in lv['before']] == [0, 50, 100] and all(apply(c, 'right', amp=0.2, dur=100)
                                                                  for c in lv['before']), lv['before']
    pt = lv['pulse_and_ttl']
    assert apply(pt[0], 'right', 110, 0.9, 40, 7), 'the pulse plays over the level'
    assert apply(pt[1], 'right', 160, 0.2, 100, 0), 'the level resumes after the pulse'
    assert apply(pt[2], 'right', 400) and pt[3] == {'t_ms': 420, 'hand': 'right', 'action': 'stop', 'amplitude': 0,
                                                    'duration_ms': 0, 'frequency_hz': 0, 'id': 0}, pt
    assert len(pt) == 4, pt
    e = r['epoch_and_disconnect']
    assert apply(e['new_epoch'][0], 'left', 100, id_=1), 'a new epoch: id 1 plays again'
    assert apply(e['new_epoch'][1], 'left', 160, 0.3, id_=0)
    assert [(c['t_ms'], c['action']) for c in e['after_disconnect']] == [(170, 'stop')], e
    assert e['block']['epoch'] == 'e2' and e['block']['last_id'] == 1
    p = r['parse']
    assert p['rejected'] == [False, False, False] and p['bad'] == 3 and p['ok'], p
    assert p['epoch'] == 'weirdepoch' and p['ttl'] == 1 and p['level'] == 1 and not p['right_present'], p
    assert p['amp'] == 1 and p['dur'] == 2 and p['delay'] == 0, p

    # -- live: the client against a stream, cut once ---------------------------------------------------------------
    def msg(seq, right_level=0.0, pulses=(), ttl=0.3):
        hands = {'left': {'level': 0.0, 'frequency_hz': 0.0, 'pulses': []},
                 'right': {'level': right_level, 'frequency_hz': 0.0, 'pulses': []}}
        for hand, id_ in pulses:
            hands[hand]['pulses'].append({'id': id_, 'group': id_, 'amplitude': 0.6, 'duration_s': 0.06,
                                          'frequency_hz': 0.0, 'delay_s': 0.0, 'age_s': 0.0})
        return {'schema': SCHEMA, 'epoch': 'live1', 'seq': seq, 'ttl_s': ttl, 'hands': hands}

    connections = []

    class Handler(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def log_message(self, *a):
            pass

        def do_GET(self):
            n = len(connections)
            connections.append(self.path)
            if self.path != '/nodes/quest_teleop/haptics?stream=1':
                self.send_response(404)
                self.send_header('Content-Length', '0')
                self.end_headers()
                return
            self.send_response(200)
            self.send_header('Content-Type', 'application/x-ndjson')
            self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()

            def send(m):
                b = (json.dumps(m) + '\n').encode()
                self.wfile.write(b'%x\r\n%s\r\n' % (len(b), b))
                self.wfile.flush()
            try:
                if n == 0:
                    send(msg(0))
                    time.sleep(0.2)
                    send(msg(1, pulses=[('left', 1), ('right', 2)]))       # armed
                    time.sleep(0.2)
                    send(msg(2, right_level=0.2, pulses=[('left', 1), ('right', 2)]))   # a level, ttl 0.3 s
                    time.sleep(0.15)
                    send(msg(3, right_level=0.2, pulses=[('left', 1), ('right', 2)]))
                    time.sleep(0.1)                                           # cut: no more keepalives
                else:                                                         # the reconnect: same pulses again
                    send(msg(4, pulses=[('left', 1), ('right', 2)]))
                    for k in range(12):
                        time.sleep(0.1)
                        send(msg(5 + k))
            except OSError:
                pass

    srv = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    url = f'http://127.0.0.1:{srv.server_address[1]}/nodes/quest_teleop/haptics?stream=1'
    live = subprocess.run([str(exe), url, '1.6'], check=True, capture_output=True, text=True, timeout=30).stdout
    srv.shutdown()
    recs = [json.loads(x) for x in live.splitlines()]
    cmds = [x['cmd'] for x in recs if x['case'] == 'cmd']
    end = recs[-1]
    pulses = [c for c in cmds if c['id'] > 0]
    assert sorted((c['hand'], c['id']) for c in pulses) == [('left', 1), ('right', 2)], \
        f'each pulse once, across the reconnect: {pulses}'
    levels = [c for c in cmds if c['id'] == 0 and c['hand'] == 'right']
    assert levels and levels[0]['action'] == 'apply' and abs(levels[0]['amplitude'] - 0.2) < 1e-6, levels
    assert levels[-1]['action'] == 'stop', f'the level stops when the stream is cut: {levels}'
    first_level, stop = levels[0]['t_ms'], levels[-1]['t_ms']
    # the last level line at ~550 ms (ttl 0.3 s), the cut at ~650 ms: the stop comes with the cut, before the TTL
    assert 200 <= stop - first_level <= 500, (first_level, stop)
    assert end['connects'] == 2 and len(connections) == 2, (end, connections)
    assert end['block'] == {'schema': SCHEMA, 'epoch': 'live1', 'last_id': 2, 'connected': True}, end['block']

print(f'PASS: haptics parsing, scheduling (once per id, groups, late pulses, levels and TTL, epochs, disconnect), '
      f'and the stream client across a reconnect ({len(cmds)} commands)')
