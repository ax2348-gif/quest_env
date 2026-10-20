#!/system/bin/sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
config="$HOME/.config/quest-env"
mkdir -p "$config"
chmod 700 "$config"
# Reject directory/symlink destinations rather than following unexpected targets.
for target in "$config/profile.sh" "$config/root"; do
    [ ! -L "$target" ] && { [ ! -e "$target" ] || [ -f "$target" ]; } || {
        echo "Refusing unexpected target: $target" >&2; exit 1;
    }
done
cp "$ROOT/shell/profile.sh" "$config/profile.sh.new"
chmod 600 "$config/profile.sh.new"
mv -f "$config/profile.sh.new" "$config/profile.sh"
printf '%s\n' "$ROOT" > "$config/root.new"
chmod 600 "$config/root.new"
mv -f "$config/root.new" "$config/root"

marker='# quest_env managed profile hook'
hook='[ ! -r "$HOME/.config/quest-env/profile.sh" ] || . "$HOME/.config/quest-env/profile.sh"'
if ! grep -qF "$marker" "$HOME/.profile" 2>/dev/null; then
    if [ -e "$HOME/.profile" ] && [ ! -e "$config/profile.before-quest-env" ]; then
        cp "$HOME/.profile" "$config/profile.before-quest-env"
    fi
    printf '\n%s\n%s\n' "$marker" "$hook" >> "$HOME/.profile"
fi
sh "$ROOT/ssh/install.sh"
echo 'Profile installed. New interactive terminals enter Termux automatically.'
echo 'Apply here: . "$HOME/.config/quest-env/profile.sh"'
