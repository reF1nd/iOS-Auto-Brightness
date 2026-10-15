#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
MODDIR=${0%/*}
STATE=/data/adb/ios_auto_brightness
echo "Module: $MODDIR"
for f in disable remove; do [ ! -e "$MODDIR/$f" ] || echo "Module flag: $f"; done
for f in bridge.jar bin/ios_brightness_daemon; do
  if [ -s "$MODDIR/$f" ]; then ls -l "$MODDIR/$f"; else echo "MISSING: $f"; fi
done
echo 'Process identity / heartbeat health:'
sh "$MODDIR/control.sh" health
echo 'Config validation:'
"$MODDIR/bin/ios_brightness_daemon" --config check "$STATE/config.conf" >/dev/null && echo valid
echo 'State files (no learning contents):'
ls -l "$STATE"/config.conf "$STATE"/learning*.dat "$STATE"/status.json 2>/dev/null
echo 'Uptime / last status modification time:'
cat /proc/uptime
stat -c %Y "$STATE/status.json" 2>/dev/null
echo 'System mode / slider (current Android user):'
settings --user current get system screen_brightness_mode
settings --user current get system screen_brightness
echo 'Battery:'
for f in capacity status temp; do printf '%s: ' "$f"; cat "/sys/class/power_supply/battery/$f" 2>/dev/null; done
echo 'Backlight nodes (read only):'
for node in /sys/class/backlight/*; do
  [ -d "$node" ] || continue
  ls -l "$node/brightness" "$node/max_brightness" 2>/dev/null
  for f in brightness actual_brightness max_brightness; do printf '%s: ' "$f"; cat "$node/$f" 2>/dev/null; done
done
echo 'Live bridge status:'
if ! sh "$MODDIR/control.sh" status; then
  echo 'OFFLINE: following snapshot may be stale:'
  cat "$STATE/status.json" 2>/dev/null
fi
echo
echo 'Recent service log:'
sh "$MODDIR/control.sh" log-read
