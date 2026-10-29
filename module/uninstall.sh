#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
STATE=/data/adb/ios_auto_brightness
[ -d "$STATE" ] && touch "$STATE/stop"
pid=$(cat "$STATE/daemon.pid" 2>/dev/null)
case "$pid" in ''|*[!0-9]*) ;; *) kill -TERM "$pid" 2>/dev/null ;; esac
node=$(cat "$STATE/backlight" 2>/dev/null)
case "$node" in /sys/class/backlight/*) chmod 0644 "$node/brightness" 2>/dev/null ;; esac
# Keep configuration and the learning offset for a reinstall; the WebUI resets learning explicitly.
