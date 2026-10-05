#!/usr/bin/env bash
# Build, test and install Dynamic Noctalia for the current user.
#
# scripts/install-local.sh [--prefix DIR] [--unit NAME] [--jobs N] [--skip-tests] [--no-restart]
# NOCTALIA_PREFIX, NOCTALIA_UNIT, NOCTALIA_BUILD_DIR and NOCTALIA_BUILD_JOBS set defaults.
# A complete previous installation is retained beside the prefix for rollback.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
prefix=${NOCTALIA_PREFIX:-$HOME/.local/lib/dynamic-noctalia}
unit=${NOCTALIA_UNIT:-dynamic-noctalia.service}
build=${NOCTALIA_BUILD_DIR:-$repo/build-release}
jobs=${NOCTALIA_BUILD_JOBS:-6}
tests=1
restart=1
while (($#)); do
  case $1 in
    --prefix|--unit|--jobs)
      (($# >= 2)) || { echo "missing value for $1" >&2; exit 2; }
      case $1 in --prefix) prefix=$2 ;; --unit) unit=$2 ;; --jobs) jobs=$2 ;; esac
      shift 2 ;;
    --skip-tests) tests=0; shift ;;
    --no-restart) restart=0; shift ;;
    -h|--help) sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
[[ $jobs =~ ^[1-9][0-9]*$ ]] || { echo 'jobs must be a positive integer' >&2; exit 2; }
[[ $unit =~ ^[a-zA-Z0-9_.@-]+\.service$ ]] || { echo 'invalid service unit name' >&2; exit 2; }
[[ -n $prefix && $prefix != *$'\n'* ]] || { echo 'invalid install prefix' >&2; exit 2; }
[[ ! -L $prefix ]] || { echo 'install prefix must not be a symlink' >&2; exit 2; }
prefix=$(realpath -m -- "$prefix")
[[ $prefix != / && $prefix != "$HOME" ]] || { echo 'use a dedicated installation directory' >&2; exit 2; }
test_mode=disabled
((tests)) && test_mode=enabled
if [[ -f $build/build.ninja ]]; then
  meson configure "$build" --prefix="$prefix" --buildtype=release -Dtests="$test_mode"
else
  meson setup "$build" --prefix="$prefix" --buildtype=release -Dtests="$test_mode"
fi
meson compile -C "$build" -j "$jobs"
if ((tests)); then
  meson test -C "$build" --no-rebuild -j "$jobs" --print-errorlogs
fi

# Finish the install in a staging directory before replacing the active tree.
install -d -- "$(dirname -- "$prefix")"
staging=$(mktemp -d "$(dirname -- "$prefix")/.dynamic-noctalia-install.XXXXXX")
trap 'rm -rf -- "$staging"' EXIT
meson install -C "$build" --no-rebuild --destdir "$staging"
[[ -x $staging$prefix/bin/noctalia && -d $staging$prefix/share/noctalia/assets ]] || {
  echo 'staged installation is incomplete' >&2; exit 1;
}
previous=
if [[ -e $prefix ]]; then
  previous=$(mktemp -d "$prefix.previous-$(date +%Y%m%d-%H%M%S).XXXXXX")
  rmdir -- "$previous"
  mv -- "$prefix" "$previous"
fi
if ! mv -- "$staging$prefix" "$prefix"; then
  [[ -z $previous ]] || mv -- "$previous" "$prefix"
  exit 1
fi
bin=$prefix/bin/noctalia
install -d "$HOME/.local/bin"
ln -sfn "$bin" "$HOME/.local/bin/noctalia"

units=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user
if [[ ! -e $units/$unit && ! -L $units/$unit ]]; then
  install -d "$units"
  python3 - "$repo/examples/systemd/dynamic-noctalia.service" "$bin" "$units/$unit" <<'PY'
from pathlib import Path
import sys
source, binary, destination = sys.argv[1:]
escaped = binary.replace('\\', '\\\\').replace('"', '\\"').replace('%', '%%').replace('$', '$$')
lines = Path(source).read_text().splitlines()
Path(destination).write_text('\n'.join('ExecStart="' + escaped + '"' if line.startswith('ExecStart=') else line for line in lines) + '\n')
PY
  echo "Wrote $units/$unit"
fi
hook=${XDG_DATA_HOME:-$HOME/.local/share}/dynamic-noctalia/start-shell.sh
if [[ ! -e $hook && ! -L $hook ]]; then
  install -Dm755 "$repo/examples/hyprland/start-shell.sh" "$hook"
  # Unit names were restricted above, so they are safe as a literal shell default.
  sed -i "s/dynamic-noctalia.service/$unit/" "$hook"
else
  echo "Kept existing start hook: $hook"
fi
systemctl --user daemon-reload
if ((restart)) && systemctl --user is-active --quiet "$unit"; then
  systemctl --user restart "$unit"
  echo "Restarted $unit"
fi
echo "Installed $bin"
[[ -z $previous ]] || echo "Previous binary and assets: $previous"
echo "Start hook: $hook"
