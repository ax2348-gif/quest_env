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
sh quest pull
sh quest run
sh quest status
```

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
