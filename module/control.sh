#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# WebUI and installer control. The core answers SIGUSR2 with status.json,
# reloads config.conf on SIGHUP and resets learning on SIGUSR1.
if [ "${IAB_CONTROL_SHELL_READY:-}" != "$$" ]; then
  export IAB_CONTROL_SHELL_READY=$$ ASH_STANDALONE=1
  for bb in /data/adb/ksu/bin/busybox /data/adb/ap/bin/busybox /data/adb/magisk/busybox; do
    [ ! -x "$bb" ] || exec "$bb" sh "$0" "$@"
  done
  echo '{"ok":false,"error":"缺少 Root 管理器 BusyBox"}'
  exit 1
fi
MODDIR=$(cd "${0%/*}" && pwd -P) || exit 1
# Never retain a root manager's temporary installer working directory.
cd / || exit 1
STATE=/data/adb/ios_auto_brightness
DAEMON="$MODDIR/bin/ios_brightness_daemon"

core_pid() {
  pid=$(cat "$STATE/daemon.pid" 2>/dev/null)
  case "$pid" in ''|*[!0-9]*) return 1 ;; esac
  tr '\000' ' ' < "/proc/$pid/cmdline" 2>/dev/null | grep -q 'ios_brightness_daemon --state' || return 1
  echo "$pid"
}
heartbeat() { sed -n 's/.*"heartbeat_ms":\([0-9]*\).*/\1/p' "$STATE/status.json" 2>/dev/null; }
pause() { sleep 0.05 2>/dev/null || usleep 50000; }
# Ask the running core for a fresh snapshot; stale files are never reported.
fresh_status() {
  pid=$(core_pid) || return 1
  before=$(heartbeat)
  kill -USR2 "$pid" 2>/dev/null || return 1
  tries=0
  while [ "$tries" -lt 40 ]; do
    pause
    now=$(heartbeat)
    if [ -n "$now" ] && [ "$now" != "$before" ]; then
      cat "$STATE/status.json"
      return 0
    fi
    tries=$((tries + 1))
  done
  return 1
}
offline() { printf '{"ok":false,"error":"%s"}\n' "$1"; }

case "$1" in
  start) exec sh "$MODDIR/start-service.sh" ;;
  status)
    fresh_status || offline '服务未运行或无响应'
    exit 0 ;;
  health)
    if fresh_status >/dev/null; then echo '{"health":"healthy"}'; else echo '{"health":"offline"}'; fi
    exit 0 ;;
  upgrade-ready)
    if snapshot=$(fresh_status); then
      beat=$(printf '%s' "$snapshot" | sed -n 's/.*"heartbeat_ms":\([0-9]*\).*/\1/p')
      printf '{"ready":true,"heartbeat_ms":%s}\n' "$beat"
    else
      echo '{"ready":false}'
    fi
    exit 0 ;;
  stop)
    touch "$STATE/stop"
    pid=$(core_pid) && kill -TERM "$pid" 2>/dev/null
    echo '{"ok":true}'
    exit 0 ;;
  config-read)
    if [ -f "$STATE/config.conf" ]; then cat "$STATE/config.conf"; else cat "$MODDIR/default.conf"; fi
    exit $? ;;
  config-save)
    umask 077
    temp=$(mktemp "$STATE/.config-input.XXXXXX") || { offline '无法写入运行目录'; exit 0; }
    trap 'rm -f "$temp"' EXIT
    printf '%s' "$2" | base64 -d > "$temp" 2>/dev/null || { offline '配置编码无效'; exit 0; }
    result=$("$DAEMON" --validate-config < "$temp") || { printf '%s\n' "$result"; exit 0; }
    chmod 600 "$temp" && mv -f "$temp" "$STATE/config.conf" || { offline '配置保存失败'; exit 0; }
    if pid=$(core_pid); then kill -HUP "$pid" 2>/dev/null; echo '{"ok":true,"applied":true}'
    else echo '{"ok":true,"applied":false}'; fi
    exit 0 ;;
  learn-reset)
    if pid=$(core_pid); then
      kill -USR1 "$pid" 2>/dev/null
    else
      rm -f "$STATE/learn.conf"
    fi
    echo '{"ok":true}'
    exit 0 ;;
  backlight)
    printf '{"ok":true,"nodes":['
    sep=
    for node in /sys/class/backlight/*; do
      [ -f "$node/brightness" ] || continue
      level=$(cat "$node/brightness" 2>/dev/null) max=$(cat "$node/max_brightness" 2>/dev/null)
      power=$(cat "$node/bl_power" 2>/dev/null) mode=$(stat -c %a "$node/brightness" 2>/dev/null)
      case "$level" in ''|*[!0-9]*) level=-1 ;; esac
      case "$max" in ''|*[!0-9]*) max=-1 ;; esac
      case "$power" in ''|*[!0-9]*) power=-1 ;; esac
      printf '%s{"path":"%s","level":%s,"max":%s,"power":%s,"mode":"%s"}' "$sep" "$node" "$level" "$max" "$power" "$mode"
      sep=,
    done
    echo ']}'
    exit 0 ;;
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
esac
offline "未知命令：$1"
exit 2
