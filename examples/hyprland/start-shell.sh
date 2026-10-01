#!/usr/bin/env bash
# Start Dynamic Noctalia from Hyprland once its display and IPC socket exist.
# Hook it up in hyprland.lua:
#   hl.on("hyprland.start", function() hl.exec_cmd("/home/you/.local/share/dynamic-noctalia/start-shell.sh") end)
set -euo pipefail
[[ -n ${HYPRLAND_INSTANCE_SIGNATURE:-} && -n ${WAYLAND_DISPLAY:-} ]] || exit 1
unit=${NOCTALIA_UNIT:-dynamic-noctalia.service}
export XDG_CURRENT_DESKTOP=Hyprland
export XDG_SESSION_TYPE=wayland
# Clear sockets left by another compositor so the shell picks the Hyprland backend.
systemctl --user unset-environment UMBRIEL_SOCKET NIRI_SOCKET SWAYSOCK LABWC_PID
dbus-update-activation-environment --systemd \
  WAYLAND_DISPLAY XDG_CURRENT_DESKTOP XDG_SESSION_TYPE HYPRLAND_INSTANCE_SIGNATURE
systemctl --user restart "$unit"
# Portals read the environment at start; restart them so screenshots and pickers work.
systemctl --user restart xdg-desktop-portal.service 2>/dev/null || true
# A polkit agent for privileged actions, if one is installed as a user unit.
systemctl --user start hyprpolkitagent.service 2>/dev/null || true
