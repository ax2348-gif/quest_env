# Quest environment delivery

This repository carries editable native source and ready-to-run Android ARM64
artifacts to the clone in the Quest runtime's workspace. The installed
`quest-dev-runtime` APK supplies SSH, terminals, the native activity, and OpenXR
loader. Routine native changes do not require rebuilding or installing that APK.

## First build on erid

```bash
cd ~/code/quest_env
python3 tools/deliver.py native
sh quest verify
git add native tools packages/telemetry quest README.md .gitignore
git commit -m "Update native telemetry"
git push origin main
```

Edit `native/telemetry.cpp`, then repeat build, verify, commit, and push. Commit
the source and corresponding package together. `packages/telemetry/build.json`
records source hashes and ABI/toolchain information; `SHA256SUMS` covers the
library and build metadata. Checksums detect corruption, not a malicious publisher:
only pull code from a repository you trust.

The builder defaults to erid's existing NDK and cached OpenXR 1.1.53 AAR. These
are build-host dependencies, not Quest dependencies. To use another machine:

```bash
python3 tools/deliver.py native \
  --ndk /path/to/android-ndk \
  --openxr-aar /path/to/openxr_loader_for_android-1.1.53.aar
```

The current builder targets Linux x86-64 development hosts. Build caches stay in
`build/` and are ignored. The small native library is intentionally tracked in
Git, so the Quest needs neither a compiler nor Git LFS.

## Pull and run on Quest

In a runtime terminal, or after SSH into the Quest:

```sh
cd ~/workspace/quest_env
# Git installed in this runtime supports HTTPS; no outbound SSH client is bundled.
git remote set-url origin https://github.com/ax2348-gif/quest_env.git
git pull --ff-only  # First pull into the previously empty clone
sh quest run
sh quest status
```

After this first pull, use `sh quest pull` for subsequent updates.
Set the HTTPS remote once. Private repositories need your normal Git HTTPS
authentication; do not commit tokens or put them into the remote URL.

`pull` requires a clean working tree and fast-forwards only. It updates and
verifies files but does not interrupt the current native app. `run` verifies the
library and asks the host to snapshot it and restart the XR process. SSH and
local terminal sessions remain running. Check status: a launch request alone is
not proof of active tracking. Live telemetry can be inspected with:

```sh
nc 127.0.0.1 9391
```

Tracking still requires the Quest to be awake, focused on XR, and past any
physical-button privacy lock. Shell/network access can work while tracking is
unavailable.

## In-headset status and shutdown

The telemetry app now displays a head-relative panel over passthrough:

- QUEST TELEMETRY title, collection/focus status, and advancing sample frame count.
- Head and left/right controller tracking status.
- Whether a telemetry TCP client is connected (collection does not require one).
- HOLD B 2 SECONDS TO SHUT DOWN, with a visible progress bar.

Hold the right controller's B button continuously for two seconds while the app
is focused. Releasing B or losing focus cancels the hold. Completion stops the
telemetry server and finishes the XR activity; SSH and terminal sessions stay
running. Start again with `sh quest run`. B remains present in the telemetry
button data, so reserve this long-hold gesture when configuring teleoperation.

The panel refreshes at 10 Hz; telemetry continues sampling on each XR frame.
The status panel was confirmed working on the headset. The first shutdown
implementation caused repeated Android "not responding" dialogs: it exited the
native worker on an OpenXR exit event before Android finished its synchronous
lifecycle callbacks. The fix keeps processing Android callbacks until
APP_CMD_DESTROY, and stops waiting for XR frames once shutdown starts. The same
fix covers initialization failure. Compilation, native-host export/architecture
checks and package checksums pass; shutdown needs another headset test.

If an older XR process is already stuck, run `quest-native stop` in the working
terminal, then `sh quest pull` and `sh quest run`. This updates only the native
library; no ADB or runtime APK update is needed.

## Stop, restart, and recovery

```sh
sh quest stop       # Stop native XR, keep terminals and SSH
sh quest restart    # Restart the selected snapshot; does not select new files
sh quest builtin    # Start the runtime's bundled known implementation
```

For a previous delivered version, use a separate Git worktree, leaving the main
checkout on its update branch:

```sh
git worktree add --detach ../quest_env-previous KNOWN_GOOD_COMMIT
sh ../quest_env-previous/quest verify
sh ../quest_env-previous/quest run
```

Replace `KNOWN_GOOD_COMMIT` with an actual tested commit. To return to current:
`sh ~/workspace/quest_env/quest run`.

## Other packages

Developer tools are a separate optional package. On erid, after building them
in `~/code/quest-dev-runtime`:

```bash
python3 tools/deliver.py devtools \
  ~/code/quest-dev-runtime/artifacts/quest-devtools-arm64.tar.gz
git add packages/devtools
git commit -m "Deliver developer tools"
git push origin main
```

On Quest, pull and then run `sh quest install-devtools`. Installation checks the
archive and installer hashes, uses versioned directories, and does not reinstall
the runtime APK. No tools archive is bundled until that packaging step is run.
Large or frequent binary releases should migrate to release assets rather than
grow Git history indefinitely.

## Scope

Native apps use host ABI v1: `quest_native_app_v1` and
`ANativeActivity_onCreate`. The included source and ABI shim were copied from
the working runtime; this repository is now the editable delivery copy for
telemetry. Keep the runtime's bundled fallback independent.

Additional permissions, Java integration, or host ABI changes still need a
runtime APK update. Arbitrary Linux/glibc binaries are not supported by this
delivery mechanism. Only trusted developer code should run inside the runtime.

## Initial validation

The initial native library built successfully and passed ARM64/export checks.
Checksums passed; a deliberately corrupted library was rejected before launch,
and the pull command refused local modifications. The existing Quest clone and
runtime were inspected over SSH. USB disconnected before the Git pull/native
launch test, and direct Tailscale SSH from erid timed out. End-to-end deployment
of this delivery commit remains to be checked on the headset.

## Default interactive environment and welcome

New runtime terminals and interactive SSH logins automatically enter the installed
`quest-termux` Bash environment. Telemetry is never started automatically. The
profile changes directory to this checkout and prints a compact command guide.
`qhelp` repeats it; `qp`, `qr`, `qs`, and `qstatus` update, run, stop, and inspect
telemetry respectively. `pkg install NAME` and `pkg upgrade` manage Termux tools.

Bootstrap from the older pull script:

```sh
cd ~/workspace/quest_env
sh quest pull
sh quest pull
. "$HOME/.config/quest-env/profile.sh"
```

The first pull obtains the installer; the second installs the profile. Subsequent
pulls re-enter the updated script and install the profile in the same invocation.
After updating an existing shell, run `qreload` or open a new terminal to load new
functions and the guide. Installation alone does not replace the current shell.

Installation appends one managed hook to `~/.profile`, preserving its existing
contents and keeping the first backup at
`~/.config/quest-env/profile.before-quest-env`. The managed profile is copied into
`~/.config/quest-env/profile.sh`; put personal additions in the optional
`~/.config/quest-env/local.sh`. Do not edit the managed copy directly. The Termux
handoff uses a dedicated Bash rcfile rather than modifying `~/.bashrc` or
`~/.bash_profile`. Shared HOME and workspace are preserved. Noninteractive SSH
commands and rsync do not enter Termux or print the welcome text.

If Termux is not installed/ready, the profile stays in the normal runtime shell.
Disable auto-entry from an existing shell (or a noninteractive SSH command) with:

```sh
touch "$HOME/.config/quest-env/no-termux"
```

Remove that file to enable it again. `QUEST_ENV_NO_TERMUX=1` also disables entry
for a shell that inherits it. Exiting the Termux shell ends that terminal session;
PRoot terminates its traced children, so don't use it for persistent daemons.

Host-side validation: `python3 test_profile.py` checks installation preservation,
idempotence, silent noninteractive startup, a single interactive Bash handoff,
welcome/reload, recovery, and that no native app is launched. The handoff test
uses a launcher stub; the installed Quest PRoot environment is tested on device
by opening a new runtime terminal after pulling.

## SSH from Quest to erid

`sh quest pull` now installs the `erid` SSH alias alongside the profile. It uses
`adrianna@100.74.177.3:22` over the existing Tailscale connection. The pinned
ED25519 server key was read directly from erid's `/etc/ssh` public key file:
`SHA256:QjEKF0zOXp//U2kEAumvN+JjLJ6jRBKFdVojXAtNlK8`.
Host verification is strict. Tailscale connectivity and sharing/ACL permissions
must allow Quest-to-erid traffic. No new VPN or server settings are required.

One-time setup in the Quest's Termux shell:

```sh
qp
pkg install openssh    # Only if not already installed
q erid-key
```

`q erid-key` creates `~/.ssh/id_ed25519_erid` only when neither key file exists.
Choose a passphrase at the prompt; the command prints only its public key.
Send that public key to erid's administrator to append to the `adrianna`
account's `~/.ssh/authorized_keys`. Erid currently offers public-key login only,
so the alias cannot log in before this enrollment. Keep the private key on Quest.
Once authorized:

```sh
ssh erid
```

The profile welcome includes these commands after `qreload`. The installer adds
one Include line at the beginning of `~/.ssh/config`, backs up any previous
configuration to `~/.ssh/config.before-quest-env`, and maintains separate
`quest-env.conf` and `quest-env-known_hosts` files. Other host entries are kept;
this managed alias takes precedence for the name `erid`. Repeated pulls do not
regenerate client keys or alter erid's server configuration.

Validated on the build host: SSH config parsing, destination/account/strict host
checking, idempotent installation, preservation of existing SSH entries and no
private-key creation during pull. Actual Quest login awaits client-key enrollment.


## Terminal alongside telemetry

`q run` now requests the existing runtime terminal panel after the first focused
XR frame. It uses a stable `teleop` session identity: restarting telemetry reuses
that terminal and shell rather than creating another each time. The shell follows
the installed profile (Termux by default). Closing the panel does not reopen it
until the next telemetry launch; closing telemetry leaves the terminal available.
If you start telemetry from another terminal, that original terminal is preserved.

Quest owns system-panel placement. On first use, use its window controls to move
and resize the terminal below your forward view, like a podium. This update does
**not** set a 3D transform for the Android window or guarantee placement survives
relaunches. Precisely positioning an embedded terminal requires additional host
APK integration; this library uses the existing host TerminalActivity.

While the terminal/system UI has input focus, controller actions report inactive,
controller poses are null, and buttons/triggers/sticks report zero. The stream
also includes `input_focused`; headset telemetry continues while XR is running.
Return focus to the immersive app before using controllers for telemetry. A native
launch intent can set boolean `open_terminal=false` to suppress automatic opening.

After pulling, run `q restart` (or `q run` if stopped). Check that the terminal
appears over passthrough, move it down, and check that returning to XR restores
controller input. Restart once to check reuse. This behavior needs on-headset
validation: if Horizon OS switches out of XR instead of showing an overlay, the
host manifest needs the cooperative hybrid-app update. No ADB is required to
pull this library; no host APK changes are bundled in this update.
