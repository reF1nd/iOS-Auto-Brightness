#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Android mksh closes high file descriptors across exec, breaking flock(9).
# KernelSU ships BusyBox ash; also use it when this script is started via adb sh.
if [ "${IAB_SHELL_READY:-}" != "$$" ]; then
  export IAB_SHELL_READY=$$ ASH_STANDALONE=1
  for bb in /data/adb/ksu/bin/busybox /data/adb/ap/bin/busybox /data/adb/magisk/busybox; do
    [ ! -x "$bb" ] || exec "$bb" sh "$0" "$@"
  done
  echo 'Root manager BusyBox is required for the supervisor lock' >&2
  exit 1
fi
MODDIR=$(cd "${0%/*}" && pwd -P) || exit 1
# Never retain a root manager's temporary installer working directory.
cd / || exit 1
STATE=/data/adb/ios_auto_brightness
[ ! -L "$STATE" ] || exit 1
umask 077
mkdir -p "$STATE"
chmod 700 "$STATE"
exec 9>"$STATE/supervisor.lock"
flock -n 9 || exit 0
LOG="$STATE/service.log"
export CLASSPATH="$MODDIR/bridge.jar"
BRIDGE_PID=
OWNED=0
cleanup() {
  trap - EXIT HUP INT TERM
  if [ -n "$BRIDGE_PID" ] && [ -r "/proc/$BRIDGE_PID/cmdline" ]; then
    cmd=$(tr '\000' ' ' < "/proc/$BRIDGE_PID/cmdline")
    case "$cmd" in *org.ref1nd.brightness.Bridge*) kill -TERM "$BRIDGE_PID" 2>/dev/null; wait "$BRIDGE_PID" 2>/dev/null ;; esac
  fi
  if [ "$OWNED" = 1 ]; then /system/bin/app_process /system/bin org.ref1nd.brightness.Bridge release >> "$LOG" 2>&1; fi
  rm -f "$STATE/supervisor.pid"
}
trap cleanup EXIT
trap 'exit 0' HUP INT TERM
printf '%s\n' "$$" > "$STATE/supervisor.pid"
while [ "$(getprop sys.boot_completed)" != 1 ]; do
  [ ! -f "$MODDIR/disable" ] && [ ! -f "$MODDIR/remove" ] || exit 0
  sleep 2
done
boot_id=$(cat /proc/sys/kernel/random/boot_id) || exit 1
[ -n "$boot_id" ] || exit 1
if [ "$(cat "$STATE/first-install-boot" 2>/dev/null)" = "$boot_id" ]; then
  echo 'First install is awaiting reboot; refusing premature service activation' >> "$LOG"
  exit 0
fi
printf '%s\n' "$boot_id" > "$STATE/activated-boot"
for id in autobrightness LiveDisplay lux-brightness adaptive-brightness brightness-controller dimmer cf.lumen twilight; do
  other="/data/adb/modules/$id"
  if [ -d "$other" ] && [ ! -f "$other/disable" ] && [ ! -f "$other/remove" ]; then
    echo "Conflict with enabled module: $id; not starting" >> "$LOG"
    exit 1
  fi
done
if [ ! -f "$STATE/config.conf" ] && [ ! -f "$STATE/legacy-config.conf" ]; then
  cp "$MODDIR/default.conf" "$STATE/config.conf"
fi
# stop is session-scoped: a deliberate reboot may restart an enabled module.
rm -f "$STATE/stop"
rotate() (
  exec 8>"$STATE/log.lock"
  flock 8 || return
  max=$(sed -n 's/^log_max_bytes=//p' "$STATE/config.conf" | tail -1)
  backups=$(sed -n 's/^log_max_backups=//p' "$STATE/config.conf" | tail -1)
  case "$max" in ''|*[!0-9]*) max=1048576 ;; esac
  case "$backups" in ''|*[!0-9]*) backups=2 ;; esac
  [ "$max" -ge 65536 ] 2>/dev/null && [ "$max" -le 8388608 ] 2>/dev/null || max=1048576
  [ "$backups" -ge 1 ] 2>/dev/null && [ "$backups" -le 8 ] 2>/dev/null || backups=2
  size=$(stat -c %s "$LOG" 2>/dev/null || echo 0)
  [ "$size" -gt "$max" ] 2>/dev/null || return
  i=$backups
  while [ "$i" -gt 1 ]; do old=$((i - 1)); [ ! -f "$LOG.$old" ] || cp "$LOG.$old" "$LOG.$i"; i=$old; done
  cp "$LOG" "$LOG.1"
  : > "$LOG"
)
failures=0
while [ ! -f "$MODDIR/disable" ] && [ ! -f "$MODDIR/remove" ] && [ ! -f "$STATE/stop" ]; do
  /system/bin/app_process /system/bin org.ref1nd.brightness.Bridge "$MODDIR" "$STATE" >> "$LOG" 2>&1 &
  BRIDGE_PID=$!
  OWNED=1
  elapsed=0
  while kill -0 "$BRIDGE_PID" 2>/dev/null; do
    [ ! -f "$MODDIR/disable" ] && [ ! -f "$MODDIR/remove" ] && [ ! -f "$STATE/stop" ] || exit 0
    sleep 2
    elapsed=$((elapsed + 2))
    [ $((elapsed % 20)) -ne 0 ] || rotate
  done
  wait "$BRIDGE_PID" 2>/dev/null
  BRIDGE_PID=
  /system/bin/app_process /system/bin org.ref1nd.brightness.Bridge release >> "$LOG" 2>&1
  if [ "$elapsed" -ge 60 ]; then failures=0; else failures=$((failures + 1)); fi
  [ "$failures" -lt 5 ] || { echo 'Repeated startup failure; leaving display with system' >> "$LOG"; exit 1; }
  sleep 5
done
