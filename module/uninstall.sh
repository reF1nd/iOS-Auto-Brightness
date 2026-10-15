#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
MODDIR=${0%/*}
STATE=/data/adb/ios_auto_brightness
[ -d "$STATE" ] && touch "$STATE/stop"
export CLASSPATH="$MODDIR/bridge.jar"
/system/bin/app_process /system/bin org.ref1nd.brightness.Bridge ctl stop 2>/dev/null
# The supervisor observes stop, exits and clears its own temporary display override.
# Keep user learning/configuration for reinstall; use WebUI to explicitly clear learning.
