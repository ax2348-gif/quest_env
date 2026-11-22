#!/usr/bin/env python3
"""Robot AR host tests: parsing/FK/placement/staleness (C++), then the native
background client against tools/fake_robot_server.py: chunked and
close-delimited streams, a separate objects stream, model_version change,
a silent stream, server loss and automatic reconnection."""
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / 'tools'))
import fake_robot_server  # noqa: E402


def run(*args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, **kwargs)


class Server:
    def __init__(self, port=0, **options):
        self.options = options
        self.start(port)

    def start(self, port):
        self.server = fake_robot_server.serve(port, **self.options)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def url(self):
        return f'http://127.0.0.1:{self.port}'

    def get(self, path):
        return urllib.request.urlopen(self.url + path, timeout=2).read()

    def stop(self):
        self.server.stopping = True
        self.server.shutdown()
        self.server.server_close()


with tempfile.TemporaryDirectory(prefix='quest-robot-ar-') as tmp:
    binary = Path(tmp) / 'robot_ar'
    run('g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread', '-I', ROOT / 'native',
        ROOT / 'test_robot_ar.cpp', '-o', binary)
    run(binary)

    for options in ({}, {'close_delimited': True}, {'objects_inline': True}):
        server = Server(**options)
        out = subprocess.run([str(binary), '--client', server.url, 'stats'], capture_output=True, text=True,
                             timeout=20)
        server.stop()
        assert out.returncode == 0, (options, out.stdout, out.stderr)
        assert 'model=fake-1' in out.stdout and 'joints=18' in out.stdout and 'markers=5' in out.stdout, out.stdout
        if options.get('objects_inline'):
            assert 'objects=2/0' in out.stdout, out.stdout
        print('PASS client', options or 'chunked', '->', ' '.join(out.stdout.split()), flush=True)

    server = Server()
    out = subprocess.run([str(binary), '--client', server.url, 'objects'], capture_output=True, text=True,
                         timeout=20)
    server.stop()
    assert out.returncode == 0 and 'objects=0/2' in out.stdout, (out.stdout, out.stderr)
    print('PASS client with a separate objects stream', flush=True)

    # Scripted outage scenario; the client prints events as they happen.
    server = Server()
    port = server.port
    proc = subprocess.Popen([str(binary), '--client', server.url, '11'], stdout=subprocess.PIPE, text=True)
    events = []

    def read():
        for line in proc.stdout:
            events.append((time.monotonic(), line.strip()))
    reader = threading.Thread(target=read, daemon=True)
    reader.start()
    t0 = time.monotonic()

    def at(seconds):
        time.sleep(max(0, t0 + seconds - time.monotonic()))
    marks = {}
    at(1.5)
    server.get('/fake/bump')
    marks['bump'] = time.monotonic()
    at(3.0)
    server.get('/fake/pause')
    marks['pause'] = time.monotonic()
    at(5.5)
    server.get('/fake/resume')
    marks['resume'] = time.monotonic()
    at(7.0)
    server.stop()
    marks['down'] = time.monotonic()
    at(8.5)
    server = Server(port)
    marks['up'] = time.monotonic()
    proc.wait(timeout=20)
    reader.join(timeout=2)
    server.stop()
    log = [e for _, e in events]
    print('\n'.join('  ' + e for e in log))

    def first(prefix, after):
        for t, e in events:
            if t >= after and e.startswith(prefix):
                return t, e
        raise AssertionError(f'no "{prefix}" after {after - t0:.1f}s: {log}')
    first('model fake-1', t0)
    t, _ = first('model fake-2', marks['bump'])
    assert t - marks['bump'] < 1.0, 'model_version change must refetch the description promptly'
    t, e = first('stale', marks['pause'])
    assert t - marks['pause'] < 1.0 and 'ROBOT AR STALE' in e, e
    t, _ = first('fresh', marks['resume'])
    assert t - marks['resume'] < 3.0, 'a silent stream reconnects (2 s stall) and resumes'
    t, e = first('stale', marks['down'])
    assert t - marks['down'] < 1.0, e
    t, _ = first('fresh', marks['up'])
    assert t - marks['up'] < 3.0, 'reconnects after the server returns'
    stop_ms = float(log[-1].split()[2])
    assert stop_ms < 500, log[-1]
    print('PASS outage: model_version refetch, stale on silence and loss, automatic reconnect,'
          f' clean stop ({stop_ms:.0f} ms)')
