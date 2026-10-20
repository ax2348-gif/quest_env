#!/usr/bin/env python3
"""Send real terminal Ctrl-C to launcher/viewer process groups."""
import errno
import os
from pathlib import Path
import pty
import select
import signal
import tempfile
import time

ROOT = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory() as tmp:
    directory = Path(tmp)
    log = directory / 'calls'
    native = directory / 'quest-native'
    native.write_text('''#!/bin/sh
echo "$1" >> "$CALL_LOG"
if [ "$1" != stop ] && [ "${SLOW_START:-0}" = 1 ]; then
    echo STARTING_NATIVE
    exec sleep 10
fi
''')
    native.chmod(0o755)
    env = dict(os.environ, PATH=str(directory)+':'+os.environ['PATH'],
               CALL_LOG=str(log), TERM='xterm-256color')

    def check(args, expected, startup=False, key=b'\x03', expected_status=130):
        log.write_text('')
        pid, fd = pty.fork()
        if pid == 0:
            os.execvpe('sh', ['sh', str(ROOT/'quest'), *args],
                       dict(env, SLOW_START='1' if startup else '0'))
        output = b''
        target = b'STARTING_NATIVE' if startup else b'QUEST TELEMETRY'
        status = None
        try:
            deadline = time.monotonic()+6
            while target not in output and time.monotonic()<deadline:
                if select.select([fd], [], [], 0.1)[0]:
                    output += os.read(fd, 65536)
            assert target in output, output
            os.write(fd, key)
            deadline = time.monotonic()+6
            while time.monotonic()<deadline:
                found, value = os.waitpid(pid, os.WNOHANG)
                if found:
                    status = value
                    break
                if select.select([fd], [], [], 0.05)[0]:
                    try:
                        output += os.read(fd, 65536)
                    except OSError as error:
                        if error.errno != errno.EIO:
                            raise
            assert status is not None, output
            assert os.waitstatus_to_exitcode(status) == expected_status, output
            assert log.read_text().splitlines() == expected, (log.read_text(), output)
        finally:
            if status is None:
                os.killpg(pid, signal.SIGKILL)
                os.waitpid(pid, 0)
            os.close(fd)

    for op in ('run', 'restart', 'builtin'):
        check([op], [op, 'stop'])
    check(['run'], ['run', 'stop'], startup=True)
    check(['watch'], [])
    check(['run'], ['run'], key=b'q', expected_status=0)
print('PASS: real Ctrl-C stops launch-owned telemetry once, including startup; watch stays read-only; q detaches')
