#!/usr/bin/env bash
# Start Dynamic Noctalia from Hyprland once its display and IPC socket exist.
# Hook it up in hyprland.lua:
#   hl.on("hyprland.start", function() hl.exec_cmd("/home/you/.local/share/dynamic-noctalia/start-shell.sh") end)
set -euo pipefail
[[ -n ${HYPRLAND_INSTANCE_SIGNATURE:-} && -n ${WAYLAND_DISPLAY:-} ]] || exit 1
unit=${NOCTALIA_UNIT:-dynamic-noctalia.service}
# UWSM imports the session environment and orders graphical-session.target.
# Plain Hyprland needs an explicit import before starting a user service.
if ! command -v uwsm >/dev/null || ! uwsm check is-active compositor-only >/dev/null 2>&1; then
  export XDG_CURRENT_DESKTOP=Hyprland XDG_SESSION_TYPE=wayland
  systemctl --user unset-environment SWAYSOCK LABWC_PID TRIAD_SOCKET MANGO_INSTANCE_SIGNATURE
  dbus-update-activation-environment --systemd \
    WAYLAND_DISPLAY XDG_CURRENT_DESKTOP XDG_SESSION_TYPE HYPRLAND_INSTANCE_SIGNATURE
fi
# Queue the start so a session hook cannot block graphical-session activation.
# Repeated calls leave an already-running shell alone.
systemctl --user --no-block start "$unit"
