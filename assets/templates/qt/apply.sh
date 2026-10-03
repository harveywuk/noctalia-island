#!/usr/bin/env bash
# qt5ct and qt6ct get the same rendered scheme, which lists 22 colour roles: Qt 6 added Accent as
# the 22nd. qt5ct ignores a scheme whose lists don't have exactly Qt 5's 21 roles, so trim the
# qt5ct copy back to 21.
set -euo pipefail

scheme="${XDG_CONFIG_HOME:-$HOME/.config}/qt5ct/colors/noctalia.conf"
[ -f "$scheme" ] || exit 0

tmp="$(mktemp "${scheme}.tmp.XXXXXX")"
trap 'rm -f "$tmp"' EXIT

awk -F', *' '
    /^(active|disabled|inactive)_colors=/ && NF > 21 {
        line = $1
        for (i = 2; i <= 21; i++) line = line ", " $i
        print line
        next
    }
    { print }
' "$scheme" > "$tmp"

if ! cmp -s "$scheme" "$tmp"; then
    cat "$tmp" > "$scheme"
fi
