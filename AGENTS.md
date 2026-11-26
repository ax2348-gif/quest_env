# AGENTS.md: context for coding agents

quest_env is the Meta Quest side of a whole-body teleop system. It runs inside the Quest developer runtime
([quest_dev](https://github.com/ax2348-gif/quest_dev): an Android app with an SSH server and a native XR host)
and streams head and controller poses, buttons, sticks and a `control_intent` as JSON on TCP port 9391. A
computer reads that stream. The consumer is [robot_utils](https://github.com/ax2348-gif/robot_utils), its
`quest_teleop` node. robot_utils' AGENTS.md describes the whole chain and how to diagnose it; start there.

## On the headset

- **Location:** `~/workspace/quest_env` (`$HOME` is the runtime's private home). `~/.config/quest-env/root` names
  it; `shell/install-profile.sh` writes it, together with a `q` shortcut for interactive shells.
- **How it arrives:**
  - **robot_utils' setup wizard** (`tools/quest_teleop/quest_setup.py`) copies the tag `collab-v4` there, runs
    `sh quest install-devtools` (git, curl, wget from `packages/devtools`), then makes it a Git clone (HTTPS
    remote) tracking branch `collab` (the maintainer's: `main`, via `--quest-env-branch`). `quest pull`
    fast-forwards it, and robot_utils' quest_up does that when the clone is behind.
  - **Without git** (`sh: git: not found`): rerun the wizard, or `sh quest install-devtools`.
- **The `quest` script** (`sh quest <cmd>` on the headset; robot_utils runs it over `ssh quest`):
  - `run [--no-viewer] [--scheme=body-frozen|standard] [-s] [--ar=URL]` starts the native telemetry; `--ar` adds
    the robot AR view (ROBOT_AR.md);
  - `stop`, `status`;
  - `verify` checks the packaged library's SHA-256 sums;
  - `watch` is a terminal viewer of the stream;
  - `pull` updates a Git clone.
- The **native library** is prebuilt and tracked: `packages/telemetry/` with `SHA256SUMS` and `build.json`. The
  headset needs no compiler.
- **Telemetry flows only while the XR session is focused.** That means the headset is awake, worn (or its
  proximity sensor covered), not privacy-locked, and nothing else is in front. SSH works in all of these states.

## Diagnosing

- `ssh quest 'sh workspace/quest_env/quest status'`: is the native process running, and its XR state.
- **The native library logs to logcat** under the tag `QuestRuntimeXR`; it has no log files of its own. The
  runtime can read logcat over SSH: `ssh quest 'logcat -d | grep QuestRuntimeXR | tail -50'`. Over USB:
  `adb logcat -s QuestRuntimeXR`.
- From the computer: robot_utils' `tools/quest_teleop/quest_status` prints the session, rate, focus, scheme and
  battery.
- **Port 9391 accepts any interface by default.** `~/.config/quest-env/tailnet-only` restricts it to loopback and
  the Tailscale address (README, "Network access").

## Building (only to change `native/`)

`python3 tools/deliver.py native` on a development computer: it needs the Android NDK 27.2 and the OpenXR loader
AAR; README, "First build". Commit the source and the rebuilt package together. The maintainer builds on a Mac
with the SDK in `~/Library/Android/sdk`. After a change, new collaborator tags must follow, and the pin in
robot_utils' `quest_setup.py` (`QUEST_ENV_REF`) with them.

## Conventions

- **`main` is what the maintainer's headset pulls:** don't push experiments there. Collaborator work goes on
  branch `collab`.
- **Don't commit tokens.** Remotes are HTTPS: the runtime has no outbound SSH client.
