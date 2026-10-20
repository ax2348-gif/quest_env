# Managed by quest_env. Personal additions belong in ~/.config/quest-env/local.sh.
# No output or auto-entry for noninteractive SSH/rsync commands.
case $- in *i*) ;; *) return 0 ;; esac

QUEST_ENV_CONFIG="$HOME/.config/quest-env"
if [ -r "$QUEST_ENV_CONFIG/root" ]; then
    IFS= read -r QUEST_ENV_ROOT < "$QUEST_ENV_CONFIG/root"
else
    QUEST_ENV_ROOT="$HOME/workspace/quest_env"
fi
export QUEST_ENV_ROOT

# Use a dedicated Bash rcfile, so login-file differences do not skip this
# profile and Termux does not recursively start another PRoot instance.
if [ "${QUEST_TERMUX_ACTIVE:-}" != 1 ] &&
   [ "${QUEST_ENV_NO_TERMUX:-}" != 1 ] &&
   [ ! -f "$QUEST_ENV_CONFIG/no-termux" ] &&
   [ -f "$HOME/.quest-termux/.ready" ] &&
   command -v quest-termux >/dev/null 2>&1; then
    exec quest-termux bash --noprofile --rcfile "$QUEST_ENV_CONFIG/profile.sh" -i
fi

if [ "${QUEST_TERMUX_ACTIVE:-}" != 1 ]; then
    export TMPDIR="$HOME/tmp"
fi
cd "$QUEST_ENV_ROOT" || return

q() { sh "$QUEST_ENV_ROOT/quest" "$@"; }
qp() { q pull; }
qr() { q run; }
qs() { q stop; }
qstatus() { q status; }
qhelp() {
    printf '\n%s\n' "Quest workspace | ${QUEST_TERMUX_ACTIVE:+Termux }shell | telemetry starts manually"
    printf '%s\n' \
      '  qp / q pull        Pull updates, verify, install profile' \
      '  qr / q run         Start updated telemetry' \
      '  qs / q stop        Stop telemetry; keep this terminal' \
      '  qstatus            Native app status' \
      '  q restart          Restart the selected native snapshot' \
      '  q builtin          Start bundled fallback telemetry' \
      '  q verify           Check telemetry package checksums' \
      '  q install-profile  Reinstall this managed profile' \
      '  q install-devtools Install optional packaged CLI tools' \
      '  pkg install NAME   Install a Termux package' \
      '  pkg upgrade        Update Termux packages' \
      '  qreload            Reload profile in this shell' \
      '  qhelp              Show this guide again' \
      '  exit               Close this shell' \
      '  In XR: hold B for 2 seconds to stop telemetry.'
    if [ "${QUEST_TERMUX_ACTIVE:-}" != 1 ]; then
        printf '%s\n' '  Termux inactive: pkg requires quest-termux to be installed/enabled.'
    fi
    printf '\n'
}
qreload() { . "$HOME/.config/quest-env/profile.sh"; }

[ ! -r "$QUEST_ENV_CONFIG/local.sh" ] || . "$QUEST_ENV_CONFIG/local.sh"
qhelp
