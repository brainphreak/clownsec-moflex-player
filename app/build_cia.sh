#!/bin/bash
# Build the installable CIA (icon + HOME-menu banner + metadata) from the ELF.
# Requires the local tools in ../tools/bin (makerom, bannertool).
set -e
cd "$(dirname "$0")"
BIN="$(cd ../tools/bin && pwd)"

export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export DEVKITARM=${DEVKITARM:-/opt/devkitpro/devkitARM}

# 1. build the ELF + SMDH (icon/metadata)
make

# 2. HOME-menu banner (256x128 image + jingle) -> banner.bin
"$BIN/bannertool" makebanner -i banner.png -a banner_audio.wav -o banner.bin

# 3. package the CIA
"$BIN/makerom" -f cia -o clownsec_player.cia \
    -elf clownsec_player.elf -rsf cia.rsf \
    -icon clownsec_player.smdh -banner banner.bin \
    -exefslogo -target t

echo "built ... clownsec_player.cia"

# 4. copy the fresh build to ~/Downloads (the location used for installing/testing) so the
#    file that gets copied to the 3DS is always the one just built, not a stale leftover.
if [ -d "$HOME/Downloads" ]; then
    cp -f clownsec_player.3dsx "$HOME/Downloads/clownsec_player.3dsx"
    cp -f clownsec_player.cia  "$HOME/Downloads/clownsec_player.cia"
    echo "copied 3dsx + cia -> $HOME/Downloads"
fi
