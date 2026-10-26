#!/data/data/com.termux/files/usr/bin/bash
# Optional after the COHOST variant has passed its first real-device test.
# Changes only the custom Termux-Desktop tx11start script's default spawn mode.
# No root, su, ADB, CPU governor or cgroup manipulation.
set -euo pipefail

target="$PREFIX/bin/tx11start"
backup="${target}.before-oneui-cohost"
case "${1:-}" in
    install)
        if ! [ -f "$target" ]; then echo "tx11start not found: $target" >&2; exit 1; fi
        if ! grep -qx 'use_topapp=true' "$target"; then
            echo 'No exact use_topapp=true default found; script already changed or uses another release.'
            exit 1
        fi
        expected_pid="$(pidof com.termux 2>/dev/null | awk '{print $1}')"
        if [ -z "$expected_pid" ] || ! logcat -d -s OneUiProcessHost:I 2>/dev/null |
            tail -n 40 | grep -q "cohost=true process=com.termux pid=$expected_pid "; then
            echo 'The currently running Termux PID is not proven to host Termux-X11. Install and validate the experimental APK first.' >&2
            exit 1
        fi
        if [ -e "$backup" ]; then echo "Backup exists, refusing overwrite: $backup" >&2; exit 1; fi
        cp -p "$target" "$backup"
        python3 - "$target" <<'PY'
import sys
from pathlib import Path
p = Path(sys.argv[1])
text = p.read_text()
assert text.count('use_topapp=true\n') == 1
p.write_text(text.replace('use_topapp=true\n', 'use_topapp=false\n', 1))
PY
        echo "Configured default native Termux start; saved backup: $backup"
        echo 'tx11start now starts XFCE in the normal Termux process tree. No per-command spawn is needed.'
        ;;
    restore)
        if [ ! -f "$backup" ]; then echo "No backup: $backup" >&2; exit 1; fi
        cp -p "$backup" "$target"
        echo "Restored: $target"
        ;;
    *)
        echo 'usage: bash disable-spawn-default.sh install|restore' >&2
        exit 2
        ;;
esac
