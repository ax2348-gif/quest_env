#!/usr/bin/env python3
"""Exercise profile installation and actual interactive Bash startup with a fake PRoot launcher."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory() as temporary:
    home = Path(temporary)
    env = dict(os.environ, HOME=str(home), QUEST_TERMUX_ACTIVE="",
               QUEST_ENV_NO_TERMUX="", PATH=f"{home}/bin:" + os.environ["PATH"])
    (home / "bin").mkdir()
    original = "# personal settings\nexport PERSONAL_SETTING=retained\n"
    (home / ".profile").write_text(original)
    for _ in range(2):
        subprocess.run(["sh", str(ROOT / "shell/install-profile.sh")],
                       env=env, check=True, capture_output=True)
    assert (home / ".profile").read_text().count("# quest_env managed profile hook") == 1
    assert (home / ".config/quest-env/profile.before-quest-env").read_text() == original
    result = subprocess.run(["sh", "-c", '. "$HOME/.profile"'],
                            env=env, check=True, capture_output=True, text=True)
    assert result.stdout == ""
    # Real interactive Bash handoff, with a stub standing in for the installed
    # quest-termux tracer. It records nesting and preserves the requested argv.
    launcher = home / "bin/quest-termux"
    launcher.write_text('#!/bin/sh\necho entered >> "$HOME/entries"\n'
                        'export QUEST_TERMUX_ACTIVE=1\nexec "$@"\n')
    launcher.chmod(0o755)
    (home / ".quest-termux").mkdir()
    (home / ".quest-termux/.ready").touch()
    native = home / "bin/quest-native"
    native.write_text('#!/bin/sh\necho unexpected >> "$HOME/native-called"\n')
    native.chmod(0o755)
    result = subprocess.run(
        ["bash", "--noprofile", "--rcfile", str(home / ".profile"), "-i"],
        input='printf "ACTIVE=%s\\n" "$QUEST_TERMUX_ACTIVE"\nqreload\nexit\n',
        env=env, capture_output=True, text=True, timeout=10, check=True)
    assert (home / "entries").read_text() == "entered\n"
    assert "ACTIVE=1" in result.stdout and "qhelp" in result.stdout
    assert not (home / "native-called").exists()
    # Recovery switch leaves the runtime shell usable without auto-entry.
    (home / ".config/quest-env/no-termux").touch()
    subprocess.run(["bash", "--noprofile", "--rcfile", str(home / ".profile"), "-i"],
                   input="exit\n", env=env, capture_output=True, text=True,
                   timeout=10, check=True)
    assert (home / "entries").read_text() == "entered\n"
print("PASS: backup, idempotence, silent noninteractive startup, one Termux entry, "
      "welcome/reload, no telemetry launch, and recovery switch")
