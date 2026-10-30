#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
MODDIR=${0%/*}
STATE=/data/adb/ios_auto_brightness
echo "Module: $MODDIR ($(sed -n 's/^version=//p' "$MODDIR/module.prop"))"
for f in disable remove update; do [ ! -e "$MODDIR/$f" ] || echo "Module flag: $f"; done
if [ -x "$MODDIR/bin/ios_brightness_daemon" ]; then ls -l "$MODDIR/bin/ios_brightness_daemon"; else echo 'MISSING: bin/ios_brightness_daemon'; fi
echo 'Supervisor / core:'
for f in supervisor.pid daemon.pid; do printf '%s: %s\n' "$f" "$(cat "$STATE/$f" 2>/dev/null || echo none)"; done
[ ! -e "$STATE/stop" ] || echo 'Stop requested for this boot'
echo 'Config validation:'
"$MODDIR/bin/ios_brightness_daemon" --validate-config < "$STATE/config.conf"
echo 'Learning anchors:'
cat "$STATE/learn.conf" 2>/dev/null || echo '(no learn.conf: system default anchors)'
echo 'System mode / slider:'
settings get system screen_brightness_mode
settings get system screen_brightness
echo 'Battery:'
for f in capacity status; do printf '%s: ' "$f"; cat "/sys/class/power_supply/battery/$f" 2>/dev/null; done
echo 'Backlight nodes:'
sh "$MODDIR/control.sh" backlight
echo 'Live status:'
sh "$MODDIR/control.sh" status
echo
echo 'Recent service log:'
sh "$MODDIR/control.sh" log-read
