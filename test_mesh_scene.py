#!/usr/bin/env python3
"""mesh_scene.h host tests (C++): outline edges, primitives, matrices, and the rigid_scene.v1 fixture written by
robot_utils (fixtures/rigid_scene)."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent


def run(*args):
    subprocess.run([str(x) for x in args], check=True)


with tempfile.TemporaryDirectory(prefix='quest-mesh-scene-') as tmp:
    binary = Path(tmp) / 'mesh_scene'
    run('g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-I', ROOT / 'native',
        ROOT / 'test_mesh_scene.cpp', '-o', binary)
    run(binary, ROOT / 'fixtures/rigid_scene')
