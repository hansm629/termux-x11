#!/data/data/com.termux/files/usr/bin/bash
# Optional post-install migration tool. Never run automatically.
# Keep the custom tx11start's other modes, audio and OneUI cohost selection intact.
set -euo pipefail
target="${TX11START_PATH:-$PREFIX/bin/tx11start}"
backup="${target}.before-integrated-gpu-controller"
case "${1:-}" in
  install)
    test -f "$target" || { echo "Missing: $target" >&2; exit 1; }
    test ! -e "$backup" || { echo "Refusing to overwrite backup: $backup" >&2; exit 1; }
    bash -n "$target"
    # Both historically distributed Termux-Desktop variants had one of these defaults.
    if ! grep -Fq 'TX11_GPU_KEEPALIVE:-auto' "$target" &&
       ! grep -Fq 'TX11_GPU_KEEPALIVE:-1' "$target"; then
      echo 'No known TX11_GPU_KEEPALIVE default; refusing to edit unknown script.' >&2
      exit 1
    fi
    cp -p "$target" "$backup"
    python3 - "$target" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text()
options = ("TX11_GPU_KEEPALIVE:-auto", "TX11_GPU_KEEPALIVE:-1")
present = [x for x in options if x in s]
assert len(present) == 1, f"Ambiguous keepalive defaults: {present}"
assert s.count(present[0]) == 1
p.write_text(s.replace(present[0], "TX11_GPU_KEEPALIVE:-0", 1))
PY
    if ! bash -n "$target"; then
      cp -p "$backup" "$target"
      echo 'Syntax failed, restored original.' >&2
      exit 1
    fi
    chmod 700 "$target"
    echo "Legacy external keepalive now defaults OFF; backup: $backup"
    echo 'Restore with: bash disable-external-keepalive.sh restore'
    ;;
  restore)
    test -f "$backup" || { echo "No backup: $backup" >&2; exit 1; }
    cp -p "$backup" "$target"
    bash -n "$target"
    echo "Restored original script: $target"
    ;;
  *)
    echo 'usage: bash disable-external-keepalive.sh install|restore' >&2
    exit 2
    ;;
esac
