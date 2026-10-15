#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
MODDIR=${0%/*}
if [ "$1" = stop ]; then touch /data/adb/ios_auto_brightness/stop; fi
case "$1" in
  config-read) cat /data/adb/ios_auto_brightness/config.conf; exit $? ;;
  config-save)
    umask 077
    temp=$(mktemp /data/adb/ios_auto_brightness/.config-input.XXXXXX) || exit 1
    trap 'rm -f "$temp"' EXIT
    printf '%s' "$2" | base64 -d > "$temp" || exit 1
    "$MODDIR/bin/ios_brightness_daemon" --config save /data/adb/ios_auto_brightness/config.conf < "$temp"
    exit $? ;;
esac
export CLASSPATH="$MODDIR/bridge.jar"
exec /system/bin/app_process /system/bin org.ref1nd.brightness.Bridge ctl "$@"
