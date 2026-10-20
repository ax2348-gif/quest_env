#!/usr/bin/env python3
"""Check default viewer launch, opt-out, noninteractive use and launch failure."""
import os
from pathlib import Path
import pty
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory() as tmp:
    directory = Path(tmp)
    log = directory / 'calls'
    for name, body in {
        'quest-native': 'echo "native $*" >> "$CALL_LOG"; exit "${NATIVE_FAILURE:-0}"',
        'python3': 'echo "python $*" >> "$CALL_LOG"',
    }.items():
        script = directory / name
        script.write_text('#!/bin/sh\n' + body + '\n')
        script.chmod(0o755)
    env = dict(os.environ, PATH=str(directory)+':'+os.environ['PATH'], CALL_LOG=str(log))

    def invoke(args, tty=True, failure=False):
        log.write_text('')
        settings = dict(env, NATIVE_FAILURE='1' if failure else '0')
        if tty:
            master, slave = pty.openpty()
            try:
                proc = subprocess.run(['sh', str(ROOT/'quest'), *args], stdin=slave,
                                      stdout=slave, stderr=slave, env=settings, timeout=5)
            finally:
                os.close(slave)
                os.close(master)
        else:
            proc = subprocess.run(['sh', str(ROOT/'quest'), *args], capture_output=True,
                                  env=settings, timeout=5)
        return proc.returncode, log.read_text()

    for op in ('run', 'restart', 'builtin'):
        code, calls = invoke([op])
        assert code == 0 and f'native {op}' in calls and 'client/watch.py' in calls, calls
        assert calls.index(f'native {op}') < calls.index('client/watch.py')
        code, calls = invoke([op, '--no-viewer'])
        assert code == 0 and 'python' not in calls, calls
        code, calls = invoke([op], tty=False)
        assert code == 0 and 'python' not in calls, calls
        code, calls = invoke([op], failure=True)
        assert code == 1 and 'client/watch.py' not in calls, calls
    code, calls = invoke(['run', '--typo'])
    assert code == 2 and not calls
print('PASS: TTY default viewer, --no-viewer, noninteractive use, native failure, invalid flags')
