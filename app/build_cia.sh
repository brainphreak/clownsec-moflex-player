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
"$BIN/makerom" -f cia -o moflex_player.cia \
    -elf moflex_player.elf -rsf cia.rsf \
    -icon moflex_player.smdh -banner banner.bin \
    -exefslogo -target t

echo "built ... moflex_player.cia"
