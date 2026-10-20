#!/system/bin/sh
# Run inside the Quest runtime shell. No Android package install or restart.
set -eu
archive="${1:?Usage: sh install_devtools.sh ARCHIVE SHA256}"
expected="${2:?Supply the archive SHA256}"
case "$expected" in *[!0-9a-f]*|"") echo "Invalid SHA256" >&2; exit 2;; esac
[ "${#expected}" -eq 64 ] || exit 2
actual="$(sha256sum "$archive")"
actual="${actual%% *}"
[ "$actual" = "$expected" ] || { echo "Archive checksum mismatch" >&2; exit 1; }
mkdir -p "$HOME/tmp" "$HOME/bin" "$HOME/.devtools/releases"
if [ -e "$HOME/devtools" ] && [ ! -L "$HOME/devtools" ]; then
    echo "Existing non-symlink $HOME/devtools; refusing to replace it." >&2
    exit 1
fi
for tool in git curl wget; do
    dest="$HOME/bin/$tool"
    if [ -e "$dest" ] || [ -L "$dest" ]; then
        [ -L "$dest" ] && [ "$(readlink "$dest")" = "../devtools/bin/$tool" ] || {
            echo "Existing custom command $dest; refusing to replace it." >&2; exit 1;
        }
    fi
done
stage="$(mktemp -d "$HOME/tmp/devtools-install.XXXXXX")"
trap 'rm -rf "$stage"' EXIT HUP INT TERM
tar -xzf "$archive" -C "$stage"
for tool in git curl wget; do
    [ -x "$stage/devtools/bin/$tool" ] || { echo "Missing $tool" >&2; exit 1; }
done
release="$HOME/.devtools/releases/$expected"
if [ ! -d "$release" ]; then
    mv "$stage" "$release"
fi
if [ -L "$HOME/devtools" ]; then
    readlink "$HOME/devtools" > "$HOME/.devtools/previous-target"
fi
# Android toybox mv follows a directory symlink even with -T. ln -n avoids
# descending into the old release when replacing the current-version link.
ln -sfn ".devtools/releases/$expected/devtools" "$HOME/devtools"
for tool in git curl wget; do
    ln -s "../devtools/bin/$tool" "$HOME/bin/.$tool-new-$$"
    mv -Tf "$HOME/bin/.$tool-new-$$" "$HOME/bin/$tool"
done
echo "Installed developer tools without restarting the runtime:"
cat "$HOME/devtools/VERSIONS.txt"
