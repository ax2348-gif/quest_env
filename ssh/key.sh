#!/system/bin/sh
set -eu
command -v ssh-keygen >/dev/null 2>&1 || {
    echo 'Install the SSH client first: pkg install openssh' >&2; exit 1;
}
mkdir -p "$HOME/.ssh"
chmod 700 "$HOME/.ssh"
key="$HOME/.ssh/id_ed25519_erid"
[ ! -L "$key" ] && [ ! -L "$key.pub" ] || {
    echo 'Refusing symlink key paths.' >&2; exit 1;
}
if [ ! -e "$key" ] && [ ! -e "$key.pub" ]; then
    echo 'Creating a Quest-only key. Choose a passphrase when prompted.'
    ssh-keygen -t ed25519 -f "$key" -C quest-to-erid
elif [ ! -f "$key" ] || [ ! -f "$key.pub" ]; then
    echo 'Incomplete key pair; refusing to overwrite it.' >&2; exit 1
fi
echo 'Send ONLY the following public key to authorize this Quest on erid:'
cat "$key.pub"
