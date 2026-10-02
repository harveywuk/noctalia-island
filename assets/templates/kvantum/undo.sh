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
    local file="$1" section="$2" key="$3" ours="$4" value="$5" tmp
    [ -f "$file" ] || return 0
    tmp="$(mktemp "${file}.tmp.XXXXXX")"
    awk -v section="[$section]" -v key="$key" -v ours="$ours" -v value="$value" '
        /^\[/ { in_section = ($0 == section); print; next }
        in_section && $0 == key "=" ours {
            if (value != "") print key "=" value
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
