#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
MODDIR=${0%/*}
if ! sh "$MODDIR/control.sh" status; then
  echo 'Bridge unavailable; last recorded state follows (may be stale):'
  cat /data/adb/ios_auto_brightness/status.json 2>/dev/null
  echo
  tail -30 /data/adb/ios_auto_brightness/service.log 2>/dev/null
  exit 1
fi
