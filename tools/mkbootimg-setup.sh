#!/bin/bash
# Install a usable `mkbootimg` for tools/mkbootimg-manaus.sh without sudo.
#
# Why this exists (2026-11-17): mkbootimg was missing in WSL, and PyPI has no
# `mkbootimg` package. The Ubuntu/Debian package script is the right one because
# it appends the header-v4 boot signature ONLY when --gki_signing_key is given,
# i.e. it emits `signature_size 0` exactly like the known-good manaus images
# (the AOSP 12.1 mkbootimg.py unconditionally appends a 4096-byte signature).
#
# The .deb ships only usr/bin/mkbootimg + usr/bin/unpack_bootimg, but the script
# imports gki.generate_gki_certificate at load time; that module is only used in
# the GKI-signing path, so a stub is enough. Both land in ~/.local/bin, which is
# NOT on PATH for non-login WSL shells: build scripts must
#   export PATH="$HOME/.local/bin:$PATH"
set -euo pipefail

DEST="$HOME/.local/bin"
mkdir -p "$DEST"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cd "$tmp"

echo "Downloading mkbootimg .deb ..."
apt-get download mkbootimg
deb=$(ls mkbootimg_*.deb | head -1)
[ -n "$deb" ] || { echo "apt-get download failed" >&2; exit 1; }

dpkg-deb -x "$deb" debx
install -m 0755 debx/usr/bin/mkbootimg "$DEST/mkbootimg"
install -m 0755 debx/usr/bin/unpack_bootimg "$DEST/unpack_bootimg"

# gki stub (not shipped by the .deb; unused by mkbootimg-manaus.sh).
mkdir -p "$DEST/gki"
: > "$DEST/gki/__init__.py"
cat > "$DEST/gki/generate_gki_certificate.py" <<'EOF'
"""Stub for the AOSP gki.generate_gki_certificate module.

The Debian mkbootimg package imports this at module load but does not ship it.
It is only reached when signing a GKI boot image (--gki_signing_key /
--boot_signature), which tools/mkbootimg-manaus.sh never does.
"""


def generate_gki_certificate(*args, **kwargs):
    raise NotImplementedError(
        "GKI certificate generation is not available (gki module stubbed)")
EOF

echo "Installed:"
ls -l "$DEST/mkbootimg" "$DEST/unpack_bootimg" "$DEST/gki"
echo
echo "Add to PATH: export PATH=\"\$HOME/.local/bin:\$PATH\""
"$DEST/mkbootimg" --help >/dev/null && echo "mkbootimg OK"
