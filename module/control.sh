#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
if [ "${IAB_CONTROL_SHELL_READY:-}" != "$$" ]; then
  export IAB_CONTROL_SHELL_READY=$$ ASH_STANDALONE=1
  for bb in /data/adb/ksu/bin/busybox /data/adb/ap/bin/busybox /data/adb/magisk/busybox; do
    [ ! -x "$bb" ] || exec "$bb" sh "$0" "$@"
  done
  echo 'Root manager BusyBox is required' >&2
  exit 1
fi
MODDIR=$(cd "${0%/*}" && pwd -P) || exit 1
# Never retain a root manager's temporary installer working directory.
cd / || exit 1
if [ "$1" = stop ]; then touch /data/adb/ios_auto_brightness/stop; fi
STATE=/data/adb/ios_auto_brightness
export CLASSPATH="$MODDIR/bridge.jar"
case "$1" in
  start) exec sh "$MODDIR/start-service.sh" ;;
  log-read)
    exec 8>"$STATE/log.lock"; flock 8 || exit 1
    # Oldest to newest, bounded output; includes context immediately after rotation.
    { i=8; while [ "$i" -ge 1 ]; do [ ! -f "$STATE/service.log.$i" ] || tail -80 "$STATE/service.log.$i"; i=$((i - 1)); done
      [ ! -f "$STATE/service.log" ] || tail -80 "$STATE/service.log"; } | tail -80
    exit $? ;;
  log-clear)
    umask 077
    exec 8>"$STATE/log.lock"; flock 8 || exit 1
    : > "$STATE/service.log" || exit 1
    i=1; while [ "$i" -le 8 ]; do rm -f "$STATE/service.log.$i" || exit 1; i=$((i + 1)); done
    echo '{"ok":true}'; exit 0 ;;
  backlight) exec /system/bin/app_process /system/bin org.ref1nd.brightness.Bridge backlight ;;
  upgrade-ready) exec /system/bin/app_process /system/bin org.ref1nd.brightness.Bridge upgrade-ready "$STATE" "$MODDIR" ;;
  health) exec /system/bin/app_process /system/bin org.ref1nd.brightness.Bridge health "$STATE" "$MODDIR" ;;
  sensors-get) exec /system/bin/app_process /system/bin org.ref1nd.brightness.Bridge sensors-get "$STATE" ;;
  sensors-save) exec /system/bin/app_process /system/bin org.ref1nd.brightness.Bridge sensors-save "$STATE" "$2" ;;
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
