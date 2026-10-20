#!/usr/bin/env python3
"""Focus recovery must resume only, and must not start a stopped XR app."""
import importlib.util
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
spec = importlib.util.spec_from_file_location('watch', Path(__file__).parent/'client/watch.py')
watch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(watch)
with patch.object(watch.subprocess, 'run', return_value=SimpleNamespace(stdout='{"ok":true,"xr_pids":[]}')) as run:
    assert watch.focus_xr()[0] is False
    assert run.call_count == 1
with patch.object(watch.subprocess, 'run', side_effect=[
    SimpleNamespace(stdout='{"ok":true,"xr_pids":[123]}'),
    SimpleNamespace(stdout='{"ok":true}')]) as run:
    assert watch.focus_xr()[0] is True
    assert [call.args[0] for call in run.call_args_list] == [
        ['quest-native', 'status'],
        ['quest-launch', 'dev.quest.runtime/android.app.NativeActivity']]
with patch.object(watch.subprocess, 'run', side_effect=FileNotFoundError('missing bridge')):
    assert watch.focus_xr()[0] is False
print('PASS: stopped app refusal, resume-only command, missing bridge handling')
