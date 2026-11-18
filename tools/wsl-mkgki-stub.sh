#!/bin/bash
# Re-create the minimal `gki` stub that Debian/Ubuntu's /usr/bin/mkbootimg
# imports at load time:
#
#   from gki.generate_gki_certificate import generate_gki_certificate
#
# The function is only *called* when --gki_signing_key / --gki_signing_algorithm
# are given, which the manaus build never does. Without this stub the import
# fails with `ModuleNotFoundError: No module named 'gki'` (the Debian package
# ships mkbootimg but not the AOSP gki python module). A WSL distro reset loses
# it again; re-run this after `apt install mkbootimg`.
#
#   wsl -d Ubuntu-24.04 -u root -- bash /mnt/d/manaus/wsl-mkgki-stub.sh
set -euo pipefail

SP=/usr/lib/python3/dist-packages
mkdir -p "$SP/gki"
: > "$SP/gki/__init__.py"
cat > "$SP/gki/generate_gki_certificate.py" <<'PYEOF'
"""Stub for the Debian mkbootimg GKI signing helper.

Imported at load time by /usr/bin/mkbootimg. The function itself is only called
when --gki_signing_key/--gki_signing_algorithm are given, which the manaus
build never does. Keeping this module lets the script import.
"""


def generate_gki_certificate(**kwargs):
    raise NotImplementedError(
        'gki.generate_gki_certificate stub: GKI boot signing is not available')
PYEOF

echo "wrote $SP/gki/generate_gki_certificate.py"
python3 -c 'from gki.generate_gki_certificate import generate_gki_certificate; print("import OK")'
mkbootimg --help >/dev/null 2>/tmp/mk.err && echo "mkbootimg --help OK" || { echo "mkbootimg failed:"; head -5 /tmp/mk.err; exit 1; }
