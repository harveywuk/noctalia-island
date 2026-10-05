#!/usr/bin/env bash
set -euo pipefail

config_home="${XDG_CONFIG_HOME:-$HOME/.config}"
kvantum_dir="$config_home/Kvantum"
theme_dir="$kvantum_dir/Noctalia"
state_file="$theme_dir/previous.conf"

previous() {
    [ -f "$state_file" ] || return 0
    awk -F= -v key="$1" '$1 == key { print substr($0, length(key) + 2); exit }' "$state_file"
}

# restore <file> <section> <key> <noctalia value> <previous value>: only undoes our own setting.
restore() {
    local file="$1" section="$2" key="$3" ours="$4" value="$5" tmp current
    [ -f "$file" ] || return 0
    # QSettings accepts spaces around keys and values. Check the effective value first so
    # a later user edit wins, including when an older apply hook left duplicate keys.
    current=$(awk -F= -v section="[$section]" -v key="$key" '
        function trim(s) { gsub(/^[[:space:]]+|[[:space:]]+$/, "", s); return s }
        /^[[:space:]]*\[/ { in_section = (trim($0) == section); next }
        in_section && trim($1) == key { value = trim(substr($0, index($0, "=") + 1)) }
        END { print value }
    ' "$file")
    [ "$current" = "$ours" ] || return 0
    tmp="$(mktemp "${file}.tmp.XXXXXX")"
    awk -F= -v section="[$section]" -v key="$key" -v value="$value" '
        function trim(s) { gsub(/^[[:space:]]+|[[:space:]]+$/, "", s); return s }
        /^[[:space:]]*\[/ { in_section = (trim($0) == section); print; next }
        in_section && trim($1) == key {
            if (!done && value != "") print key "=" value
            done = 1
            next
        }
        { print }
    ' "$file" > "$tmp"
    if ! cmp -s "$file" "$tmp"; then
        cat "$tmp" > "$file"
    fi
    rm -f "$tmp"
}

restore "$kvantum_dir/kvantum.kvconfig" General theme Noctalia "$(previous kvantum_theme)"
restore "$config_home/qt5ct/qt5ct.conf" Appearance style kvantum "$(previous qt5ct_style)"
restore "$config_home/qt6ct/qt6ct.conf" Appearance style kvantum "$(previous qt6ct_style)"

rm -rf -- "$theme_dir"
