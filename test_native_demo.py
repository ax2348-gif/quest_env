#!/usr/bin/env python3
"""Host control/IK tests and real GLES preview with mock OpenXR swapchains."""
import ctypes.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
NDK = Path(os.environ.get('ANDROID_NDK_HOME', '/tmp/teleop_tools/android/ndk/27.2.12479018'))

def run(*args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, **kwargs)

with tempfile.TemporaryDirectory(prefix='quest-demo-test-') as tmp:
    tmp = Path(tmp)
    binary = tmp / 'core'
    run('g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-I', ROOT/'native',
        ROOT/'test_teleop_core.cpp', '-o', binary)
    run(binary)
    intent = json.loads(subprocess.check_output([str(binary), '--json']))
    assert intent['schema'] == 'teleop.operator_intent.v1'
    assert intent['hands']['left']['engaged']
    assert intent['hands']['left']['engagement_id'] == 1
    assert intent['hands']['left']['delta_position'] == [0.1, 0, 0]
    assert not intent['hands']['right']['engaged']
    assert len(intent['hands']['left']['delta_rotation']) == 9
    print('PASS: serialized intent schema and independent hands', flush=True)
    egl, gles = ctypes.util.find_library('EGL'), ctypes.util.find_library('GLESv2')
    headers = NDK/'toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include'
    xr = ROOT/'build/native-app/openxr/prefab/modules/headers/include'
    if not egl or not gles or not headers.exists() or not xr.exists():
        raise SystemExit('GLES preview needs host EGL/GLES libraries, NDK and a native build first.')
    for name in ('EGL', 'GLES3', 'KHR'):
        shutil.copytree(headers/name, tmp/'include'/name)
    render = tmp / 'render'
    run('g++', '-std=c++17', '-O2', '-DWL_EGL_PLATFORM', '-I', tmp/'include',
        '-I', xr, ROOT/'test_demo_render.cpp', '-l:'+egl, '-l:'+gles, '-o', render)
    run(render, env=dict(os.environ, LIBGL_ALWAYS_SOFTWARE='1'))
    print('Previews: /tmp/quest-demo-eye-{1,2}.ppm (synthetic camera, not headset capture).')
