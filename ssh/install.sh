#!/system/bin/sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
mkdir -p "$HOME/.ssh"
chmod 700 "$HOME/.ssh"
for name in config quest-env.conf quest-env-known_hosts; do
    target="$HOME/.ssh/$name"
    [ ! -L "$target" ] && { [ ! -e "$target" ] || [ -f "$target" ]; } || {
        echo "Refusing unexpected SSH destination: $target" >&2; exit 1;
    }
done
cp "$ROOT/ssh/config" "$HOME/.ssh/quest-env.conf"
cp "$ROOT/ssh/erid_known_hosts" "$HOME/.ssh/quest-env-known_hosts"
chmod 600 "$HOME/.ssh/quest-env.conf" "$HOME/.ssh/quest-env-known_hosts"
include='Include ~/.ssh/quest-env.conf'
if ! grep -qxF "$include" "$HOME/.ssh/config" 2>/dev/null; then
    if [ -f "$HOME/.ssh/config" ] && [ ! -e "$HOME/.ssh/config.before-quest-env" ]; then
        cp "$HOME/.ssh/config" "$HOME/.ssh/config.before-quest-env"
    fi
    temporary="$(mktemp "$HOME/.ssh/config.XXXXXX")"
    {
        printf '%s\n' '# quest_env managed SSH hosts' "$include" ''
        [ ! -f "$HOME/.ssh/config" ] || cat "$HOME/.ssh/config"
    } > "$temporary"
    chmod 600 "$temporary"
    mv -f "$temporary" "$HOME/.ssh/config"
fi
echo 'SSH alias installed: ssh erid (adrianna@100.74.177.3)'
