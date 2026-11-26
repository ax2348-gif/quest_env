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
    env = dict(os.environ, HOME=str(directory), PATH=str(directory)+':'+os.environ['PATH'], CALL_LOG=str(log))

    def invoke(args, tty=True, failure=False):
        log.write_text('')
        (directory / '.config/quest-env/restore-terminal-once').unlink(missing_ok=True)
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
        request = directory / '.config/quest-env/restore-terminal-once'
        rejected = '--typo' in args or any(a.startswith('--ar') for a in args) and proc.returncode == 2
        expected_request = tty and not failure and '--no-viewer' not in args and not rejected
        assert request.exists() == expected_request, (args, request.exists())
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
    for args, mode in [(['run','-s'],'live'), (['run','-s','--base-frame=frozen'],'frozen'),
                       (['run','-s','--scheme=body-frozen'],'live body-frozen'), (['run','--scheme=body-frozen'],'off body-frozen'), (['restart','-s','--no-viewer'],'live'), (['run'],'off')]:
        code, calls = invoke(args)
        assert code == 0, calls
        assert (directory / '.config/quest-env/demo-mode').read_text().strip() == mode
    code, calls = invoke(['run', '--typo'])
    assert code == 2 and not calls
    # Robot AR view: a config file per launch; absent without --ar.
    ar = directory / '.config/quest-env/robot-ar'
    code, calls = invoke(['run', '--scheme=body-frozen', '--ar=http://erid:8080', '--ar-offset=0,0.5,0',
                          '--ar-yaw=90', '--ar-scale=0.5', '--no-viewer'])
    assert code == 0, calls
    assert ar.read_text() == 'url=http://erid:8080\noffset=0,0.5,0\nyaw=90\nscale=0.5\n', ar.read_text()
    assert (directory / '.config/quest-env/demo-mode').read_text().strip() == 'off body-frozen'
    assert oct(ar.stat().st_mode & 0o777) == '0o600'
    code, calls = invoke(['run', '--sim-ar=http://10.0.0.5:80', '--ar-state=http://robot:8200/robot_state/stream',
                          '-s', '--no-viewer'])
    assert code == 0 and 'state=http://robot:8200/robot_state/stream' in ar.read_text(), calls
    code, calls = invoke(['run', '--no-viewer'])
    assert code == 0 and not ar.exists()
    for bad in (['--ar=https://x'], ['--ar=http://a b'], ['--ar=http://a\nstate=http://evil'], ['--ar-yaw=5'],
                ['--ar='], ['--ar-model=ftp://x', '--ar=http://a']):
        code, calls = invoke(['run', '--no-viewer', *bad])
        assert code == 2 and not calls and not ar.exists(), bad
    code, calls = invoke(['builtin', '--no-viewer', '--ar=http://a'])
    assert code == 2 and not calls
    # Body tracking: a config file per launch; absent without --body.
    body = directory / '.config/quest-env/body-tracking'
    code, calls = invoke(['run', '--body', '--no-viewer'])
    assert code == 0 and body.read_text() == 'joints=core\n', body.read_text()
    assert oct(body.stat().st_mode & 0o777) == '0o600'
    code, calls = invoke(['run', '--body-set=default', '--body=all', '--body-fidelity=low', '--body-every=2',
                          '--no-viewer'])
    assert code == 0 and body.read_text() == 'joint_set=default\njoints=all\nfidelity=low\nevery=2\n', body.read_text()
    code, calls = invoke(['run', '--no-viewer'])
    assert code == 0 and not body.exists()
    for bad in (['--body-fidelity=high'], ['--body=legs'], ['--body', '--body-every=0'], ['--body-every=1000']):
        code, calls = invoke(['run', '--no-viewer', *bad])
        assert code == 2 and not calls and not body.exists(), bad
    code, calls = invoke(['builtin', '--no-viewer', '--body'])
    assert code == 2 and not calls
print('PASS: TTY default viewer, --no-viewer, noninteractive use, native failure, invalid flags, robot AR config, '
      'body tracking config')
