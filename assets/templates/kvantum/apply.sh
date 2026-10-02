#!/usr/bin/env bash
# Builds the "Noctalia" Kvantum theme from Kvantum's own macOS-style KvMojave theme, recoloured to
# the palette and resized by the rendered overrides, then selects it for Qt 5 and Qt 6 apps.
set -euo pipefail

mode="${1:-dark}"
config_home="${XDG_CONFIG_HOME:-$HOME/.config}"
kvantum_dir="$config_home/Kvantum"
theme_dir="$kvantum_dir/Noctalia"
overrides="$theme_dir/overrides.kvconfig"
state_file="$theme_dir/previous.conf"

[ "$mode" = "light" ] && base_name="KvMojaveLight" || base_name="KvMojave"

find_base_theme() {
    local -a dirs=("$kvantum_dir")
    local data_dirs="${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
    IFS=':' read -ra extra <<< "$data_dirs"
    for dir in "${extra[@]}"; do
        [ -n "$dir" ] && dirs+=("$dir/Kvantum")
    done
    for dir in "${dirs[@]}"; do
        if [ -f "$dir/$base_name/$base_name.kvconfig" ] && [ -f "$dir/$base_name/$base_name.svg" ]; then
            printf '%s\n' "$dir/$base_name"
            return 0
        fi
    done
    return 1
}

if [ ! -f "$overrides" ]; then
    echo "Error: Kvantum overrides not found at $overrides" >&2
    exit 1
fi

if ! base_dir="$(find_base_theme)"; then
    echo "Kvantum theme $base_name not found; install Kvantum to give Qt apps macOS-style controls" >&2
    exit 0
fi

override_value() {
    awk -F= -v key="$1" '
        /^\[/ { in_section = ($0 == "[%Noctalia]") ; next }
        in_section && $1 == key { print $2; exit }
    ' "$overrides"
}

accent="$(override_value accent)"
accent_dark="$(override_value accent_dark)"
accent_light="$(override_value accent_light)"
window="$(override_value window)"

# KvMojave draws selected menu rows, checked boxes, slider fills and focus in these blues. Windows
# and dialogs lose its gradient for the flat palette surface, selected list rows its grey for the
# accent, and slider knobs turn white as on macOS. KvMojave's SVG has one element per line (or one
# small group), so the per-element edits are line-scoped.
sed -e "s/#3273c3/$accent/gI" -e "s/#295e9f/$accent_dark/gI" -e "s/#77aff1/$accent_light/gI" \
    -e "s/#286adc/$accent/gI" -e "s/#245fc4/$accent_dark/gI" \
    -e "/id=\"\(window\|dialog\)-normal/ { s/opacity:[^;\"]*;\?//; s/fill:[^;\"]*/fill:$window/ }" \
    -e "/id=\"slidercursor-\(normal\|focused\)\"/,/<\/g>/ { /opacity:/! s/fill:[^;\"]*/fill:#ffffff;stroke:#000000;stroke-opacity:0.12;stroke-width:0.5/ }" \
    -e "/id=\"itemview-\(toggled\|pressed\)/ s/fill:[^;\"]*/fill:$accent/" \
    "$base_dir/$base_name.svg" > "$theme_dir/Noctalia.svg.tmp"
mv -f "$theme_dir/Noctalia.svg.tmp" "$theme_dir/Noctalia.svg"

# Merge: keys in the overrides replace the base theme's, keys it lacks are appended to their section.
awk '
    function flush(section,    key) {
        for (key in pending) {
            split(key, parts, SUBSEP)
            if (parts[1] == section) {
                print parts[2] "=" pending[key]
                delete pending[key]
            }
        }
    }
    FNR == NR {
        if ($0 ~ /^\[/) { section = $0; next }
        if (section == "[%Noctalia]" || $0 ~ /^[[:space:]]*(#|$)/) next
        eq = index($0, "=")
        if (eq > 0) {
            pending[section, substr($0, 1, eq - 1)] = substr($0, eq + 1)
            sections[section] = 1
        }
        next
    }
    /^[[:space:]]*$/ { blanks++; next }
    /^\[/ {
        if (current != "") flush(current)
        for (; blanks > 0; blanks--) print ""
        current = $0
        seen[current] = 1
        print
        next
    }
    {
        for (; blanks > 0; blanks--) print ""
        eq = index($0, "=")
        if (current != "" && eq > 0) {
            key = substr($0, 1, eq - 1)
            if ((current, key) in pending) {
                print key "=" pending[current, key]
                delete pending[current, key]
                next
            }
        }
        print
    }
    END {
        if (current != "") flush(current)
        for (; blanks > 0; blanks--) print ""
        for (section in sections) {
            if (!(section in seen)) {
                print ""
                print section
                flush(section)
            }
        }
    }
' "$overrides" "$base_dir/$base_name.kvconfig" > "$theme_dir/Noctalia.kvconfig.tmp"
mv -f "$theme_dir/Noctalia.kvconfig.tmp" "$theme_dir/Noctalia.kvconfig"

# ini_get <file> <section> <key>
ini_get() {
    [ -f "$1" ] || return 0
    awk -F= -v section="[$2]" -v key="$3" '
        /^\[/ { in_section = ($0 == section); next }
        in_section && $1 == key { print substr($0, length(key) + 2); exit }
    ' "$1"
}

# ini_set <file> <section> <key> <value>: writes through symlinks, keeps everything else.
ini_set() {
    local file="$1" section="$2" key="$3" value="$4" tmp
    mkdir -p "$(dirname "$file")"
    [ -e "$file" ] || : > "$file"
    tmp="$(mktemp "${file}.tmp.XXXXXX")"
    awk -v section="[$section]" -v key="$key" -v value="$value" '
        /^\[/ {
            if (in_section && !done) { print key "=" value; done = 1 }
            in_section = ($0 == section)
            if (in_section) found = 1
            print
            next
        }
        in_section && index($0, key "=") == 1 {
            if (!done) { print key "=" value; done = 1 }
            next
        }
        { print }
        END {
            if (!done) {
                if (!found) print section
                print key "=" value
            }
        }
    ' "$file" > "$tmp"
    if ! cmp -s "$file" "$tmp"; then
        cat "$tmp" > "$file"
    fi
    rm -f "$tmp"
}

kvantum_conf="$kvantum_dir/kvantum.kvconfig"
qt5ct_conf="$config_home/qt5ct/qt5ct.conf"
qt6ct_conf="$config_home/qt6ct/qt6ct.conf"

# Remember what the user had, once, so undo.sh can put it back.
if [ ! -f "$state_file" ]; then
    {
        echo "[previous]"
        echo "kvantum_theme=$(ini_get "$kvantum_conf" General theme)"
        echo "qt5ct_style=$(ini_get "$qt5ct_conf" Appearance style)"
        echo "qt6ct_style=$(ini_get "$qt6ct_conf" Appearance style)"
    } > "$state_file"
fi

ini_set "$kvantum_conf" General theme Noctalia
# qt5ct/qt6ct pick the widget style; only touch configs that already exist.
for conf in "$qt5ct_conf" "$qt6ct_conf"; do
    [ -f "$conf" ] && ini_set "$conf" Appearance style kvantum
done

echo "Kvantum theme Noctalia applied (based on $base_name)"
