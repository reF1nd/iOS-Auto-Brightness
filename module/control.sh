#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
MODDIR=${0%/*}
if [ "$1" = stop ]; then touch /data/adb/ios_auto_brightness/stop; fi
export CLASSPATH="$MODDIR/bridge.jar"
exec /system/bin/app_process /system/bin org.ref1nd.brightness.Bridge ctl "$@"
