#!/usr/bin/env bash
# Install a built snapshot without changing the active profile or service state.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
bundle="$HOME/.local/lib/noctalia-island"
install -d "$bundle/bin" "$bundle/.deps/usr/lib" "$HOME/.local/bin"
install -m755 "$repo/build-island/noctalia" "$bundle/bin/noctalia.new"
strip --strip-debug "$bundle/bin/noctalia.new"
mv -f "$bundle/bin/noctalia.new" "$bundle/bin/noctalia"
cp -L "$repo/.deps/usr/lib/libqalculate.so.23" "$bundle/.deps/usr/lib/libqalculate.so.23"
cp -a "$repo/assets" "$bundle/"
install -m644 "$repo/LICENSE" "$bundle/LICENSE"
cat > "$HOME/.local/bin/noctalia" <<'WRAPPER'
#!/bin/sh
export NOCTALIA_CONFIG_HOME="$HOME/.local/state/noctalia-island/config"
export NOCTALIA_STATE_HOME="$HOME/.local/state/noctalia-island/state"
exec "$HOME/.local/lib/noctalia-island/bin/noctalia" "$@"
WRAPPER
chmod 755 "$HOME/.local/bin/noctalia"
printf 'Installed %s\nRestart with: systemctl --user restart noctalia-island.service\n' "$bundle"
