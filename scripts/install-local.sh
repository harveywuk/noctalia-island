#!/usr/bin/env bash
# Build, test and install Dynamic Noctalia for the current user.
#
#   scripts/install-local.sh [--prefix DIR] [--unit NAME] [--skip-tests] [--no-restart]
#
# Defaults: prefix ~/.local/lib/dynamic-noctalia, unit dynamic-noctalia.service,
# build directory build-release (NOCTALIA_PREFIX / NOCTALIA_UNIT / NOCTALIA_BUILD_DIR
# override them). The previous binary is kept as bin/noctalia.previous-<timestamp>
# (the newest five are retained). A user unit is written only if none exists, and a
# running unit is restarted so the new build takes effect.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
prefix=${NOCTALIA_PREFIX:-$HOME/.local/lib/dynamic-noctalia}
unit=${NOCTALIA_UNIT:-dynamic-noctalia.service}
build=${NOCTALIA_BUILD_DIR:-$repo/build-release}
tests=1
restart=1
while (($#)); do
  case $1 in
    --prefix) prefix=$2; shift 2 ;;
    --unit) unit=$2; shift 2 ;;
    --skip-tests) tests=0; shift ;;
    --no-restart) restart=0; shift ;;
    -h|--help) sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

if [[ -f $build/build.ninja ]]; then
  meson configure "$build" --prefix="$prefix" >/dev/null
else
  meson setup "$build" --prefix="$prefix" --buildtype=release -Dtests=enabled
fi
meson compile -C "$build"
((tests)) && meson test -C "$build" --no-rebuild --print-errorlogs

bin=$prefix/bin/noctalia
if [[ -x $bin ]]; then
  cp -p "$bin" "$bin.previous-$(date +%Y%m%d-%H%M%S)"
  ls -1t "$bin".previous-* 2>/dev/null | tail -n +6 | xargs -r rm -f --
fi
meson install -C "$build" --no-rebuild
install -d "$HOME/.local/bin"
ln -sfn "$bin" "$HOME/.local/bin/noctalia"

units=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user
if [[ ! -f $units/$unit ]]; then
  install -d "$units"
  sed "s|^ExecStart=.*|ExecStart=$bin|" "$repo/examples/systemd/dynamic-noctalia.service" >"$units/$unit"
  echo "Wrote $units/$unit"
fi
systemctl --user daemon-reload
if ((restart)) && systemctl --user is-active --quiet "$unit"; then
  systemctl --user restart "$unit"
  echo "Restarted $unit"
fi
echo "Installed $bin"
echo "Roll back: cp -p \"\$(ls -1t $bin.previous-* | head -1)\" $bin && systemctl --user restart $unit"
