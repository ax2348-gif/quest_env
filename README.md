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

## In-headset HUD and shutdown

The opaque panel is replaced by premultiplied-alpha green graphics over passthrough:

- A title and shutdown hint stay for four seconds, then fade over two seconds.
- A faint, small green plus crosshair remains in the centre; it is a visual
  activity indicator, not a targeting/control input.
- Peripheral status shows head/controller tracking, stream connection, the XR
  predicted display rate, and input-poll elapsed time in milliseconds.
- Small view-facing rings follow the tracked controller grips, offset 4 cm up.
  A dot shows thumbstick deflection and an arc shows grip/squeeze. These are
  approximate controller markers, not calibrated physical thumbstick locations.
  They disappear when tracking or XR focus is lost.

`INPUT ... MS` measures elapsed time for action sync, pose reads and diagnostic
formatting, before JSON serialization/socket sending. It is **not end-to-end
latency**. `XR ... HZ` is derived from the runtime's predicted display period,
not measured receiver throughput; the TUI separately reports receive rate.

Hold right B for two seconds while focused to stop telemetry. A progress arc and
cancel hint appear during the hold. Releasing B or losing focus cancels it.
SSH and terminal sessions survive; B remains part of the telemetry button data.
Artwork updates at 10 Hz; layer poses and telemetry update on every XR frame.

Terminal restoration is now an explicit one-shot CLI request. After native
startup, `qr` creates `~/.config/quest-env/restore-terminal-once`; the native app
consumes it before re-showing the existing terminal. It does not restore panels
in response to gaining XR focus. Consuming the request on disk also prevents
activity/process recreation from re-arming restoration. Stop/Ctrl-C clears any
pending request. `--no-viewer` creates no restoration request.

The latest fresh-ACTION_VIEW experiment was reverted after reports of worse
minimization and gaze-relative panel movement. Restoration again uses the saved
document intent with adjacent-panel flags. The one-shot request prevents our
code from repeatedly restoring windows, but does not itself control Horizon OS
window anchoring. Whether activity recreation caused the observed movement is
not yet verified on device.

The TUI exposes `XR: VISIBLE/FOCUSED/...` and action-sync status (`OK`,
`NOT_FOCUSED`, or a result code). Inputs remain gated on XR focus and successful
sync. Actual panel visibility/focus must be checked on Quest; the host build
cannot validate compositor behavior.

The previous shutdown lifecycle fix remains: pump Android callbacks until
APP_CMD_DESTROY and stop waiting for XR frames once finishing starts.

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

**Prerequisite: enable Seamless Multitasking in Quest Settings.** The user
confirmed on 2026-10-20 that this fixes controller focus with the terminal
visible. With it disabled, even a minimized panel can keep XR `NOT_FOCUSED`;
closing the last panel restores input. After enabling it, use Meta to return
input to XR while leaving the viewer visible.

Validated combination: runtime APK 1.5.2 (version code 9), telemetry `3d1ca98`.
The system menu's missing app name remains unresolved and did not prevent this
working combination.

`qr` / `q run`, `q restart`, and `q builtin` start the viewer in the **invoking
terminal** after the native launch succeeds. They do not create a new terminal.
`qr --no-viewer` (or `q run --no-viewer`) skips the viewer. The same flag works
for restart/builtin. Noninteractive invocations skip it automatically. `qr`
forwards its arguments after `qreload` or opening a fresh terminal.

Quest owns panel placement. Use its window controls to place the terminal below
the forward view like a podium. No system-window transform is forced. Use the
Meta button to return input focus to XR; the viewer keeps updating while the
terminal is unfocused. If XR stays unfocused, the new diagnostics show that
explicitly rather than presenting inactive controllers as a tracking success.

## Terminal telemetry viewer

On Quest:

```sh
q pull
qreload
pkg install python   # once, in Termux; skip if already installed
qr                   # load updated library, then show viewer here
```

Use **`q run` / `qr`** after pulling to select the updated library; `q restart`
only restarts the previously selected snapshot. For separate control:

```sh
qr --no-viewer
q watch
```

The standard-library Python/curses dashboard displays receive rate, sample age,
XR input focus, headset and controller positions/rotation matrices, tracking
validity, triggers, grips, sticks, and buttons. Poses are raw OpenXR LOCAL
coordinates in metres, +Y up; R maps device axes into LOCAL. It is a read-only
viewer, not a command source. Press the Meta button to restore XR input focus;
the dashboard continues updating. `q`/Esc detaches without stopping telemetry;
Ctrl-C also stops telemetry when the viewer was launched by `qr`/`q run`.
Standalone `q watch` never stops telemetry. Resize the terminal or use Up/Down (j/k) to scroll smaller windows.

The client retries connections, marks samples stale after one second, and clears
old poses on disconnect. UI refresh is 10 Hz while the socket drains full-rate
samples. `q watch --host HOST --port PORT` can view an explicitly forwarded stream;
the native server still binds only to 127.0.0.1:9391. It now supports eight readers,
so the TUI can coexist with teleop. Slow readers are disconnected independently
rather than blocking XR; excess connections are closed. No recording or automatic
telemetry startup is enabled by the viewer.


Host validation for this update: Android ARM64 build/export/checksum checks;
`test_launch.py` verifies TTY auto-viewer, opt-out, noninteractive invocation and
failure behavior; `test_tui.py` verifies framing/reconnect/stale state and curses;
`test_controller_server.py` verifies reader isolation. HUD rectangle rendering
was inspected with a software capture of the actual drawing calls, including
transparent backgrounds, premultiplied alpha and title fading. This does not
replace checking compositor placement, readability and controller focus on Quest.


### Ctrl-C ownership

When `qr` / `q run`, `q restart` or `q builtin` launches the viewer, Ctrl-C
exits the viewer **and stops native telemetry**, returning status 130. This also
applies during native startup. The supervising shell sends the stop request and
ignores repeated interrupts while it completes. SSH and terminal sessions remain
running. A failed stop is reported with a `q stop` recovery hint.

`q` or Esc deliberately detaches the viewer and leaves telemetry running.
Standalone `q watch` remains read-only: Ctrl-C exits only that viewer.
`--no-viewer` and noninteractive launches retain their detached behavior.


### Explicit XR focus recovery

`q focus` is a diagnostic only: it hides the CLI on the tested Quest and does
not satisfy simultaneous terminal visibility and XR control. The viewer no
longer advertises or binds the f shortcut. This checks that an XR process exists
then uses `quest-launch` to resume `android.app.NativeActivity`, whose host
manifest declares `singleTask`. It does not invoke `quest-native run/restart`,
stop the process, or intentionally reset reference frames. The one-time panel
restoration has already run, so it will not immediately open the panel again.

Success means Android accepted the resume request, not proof of restored XR
focus. Check for `XR: FOCUSED | actions: OK` and active controllers. Quest may
hide the system panel when resuming XR; simultaneous panel visibility and XR
focus are still controlled by Horizon OS. If this fails, further changes belong
in the host APK's hybrid/focus-aware activity configuration, not controller math.
The viewer's f key is an explicit local UI-focus action; normal telemetry viewing
and standalone viewer exit still do not start/stop the native app.


### Panel restoration investigation

The earlier in-memory once flag reset whenever the XR app object was recreated.
A disk-backed request now survives that boundary by being consumed before launch.
No request is generated by Meta-button or focus events. This removes a possible
reopening loop; it is not proof that such a loop occurred on the headset.

## Optional native kinematics demo

Run `qp`, then `qr -s` for the immersive wireframe bimanual mobile manipulator.
Each arm has 7 DOF; grips clutch independently, triggers close grippers, left
stick translates and right stick X turns the base. Y resets the demo.
`qr -s --base-frame=frozen` freezes robot-base frames at clutch engagement; the
default tracks the live base. Plain `qr` disables the demo.

The library now also emits `control_intent` alongside raw telemetry, including
per-hand clutch IDs and operator-base translation/rotation deltas. Read
[DEMO.md](DEMO.md) for the protocol, remote anchor handling, controls and validation
limits. No APK update is needed.
