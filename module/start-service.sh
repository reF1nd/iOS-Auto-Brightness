#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Explicit WebUI recovery only. Nothing remains resident after disable.
if [ "${IAB_START_SHELL_READY:-}" != "$$" ]; then
  export IAB_START_SHELL_READY=$$ ASH_STANDALONE=1
  for bb in /data/adb/ksu/bin/busybox /data/adb/ap/bin/busybox /data/adb/magisk/busybox; do
    if [ -x "$bb" ]; then export IAB_START_BB="$bb"; exec "$bb" sh "$0"; fi
  done
  echo '{"ok":false,"error":"缺少 Root 管理器 BusyBox，无法启动服务"}'
  exit 0
fi
MODDIR=$(cd "${0%/*}" && pwd -P) || exit 1
STATE=/data/adb/ios_auto_brightness
cd / || exit 1
umask 077
fail() { printf '{"ok":false,"error":"%s"}\n' "$1"; exit 0; }
guard() {
  [ "$MODDIR" = /data/adb/modules/ios_auto_brightness ] || fail '只能从已安装模块启动服务'
  [ ! -e "$MODDIR/disable" ] || fail '模块仍被禁用，请先在 Root 管理器中启用'
  [ ! -e "$MODDIR/remove" ] || fail '模块待卸载，不能启动服务'
  [ "$(getprop sys.boot_completed)" = 1 ] || fail '系统尚未完成启动，请稍后重试'
  boot_id=$(cat /proc/sys/kernel/random/boot_id) || fail '无法确认开机状态'
  [ -n "$boot_id" ] || fail '无法确认开机状态'
  first=$(cat "$STATE/first-install-boot" 2>/dev/null)
  [ "$first" != "$boot_id" ] || fail '首次安装尚未重启，请先重启手机'
  [ -s "$STATE/activated-boot" ] || [ -n "$first" ] || fail '尚无模块激活记录，请先重启手机'
  [ ! -e "$MODDIR/update" ] || fail '模块有待完成的更新，请等待安装完成或重启'
  for file in bridge.jar service.sh control.sh SOURCE.txt; do
    [ -s "$MODDIR/$file" ] || fail '模块文件不完整，请重新安装'
  done
  [ -x "$MODDIR/bin/ios_brightness_daemon" ] || fail '核心文件不可执行，请重新安装'
  for id in autobrightness LiveDisplay lux-brightness adaptive-brightness brightness-controller dimmer cf.lumen twilight; do
    other="/data/adb/modules/$id"
    if [ -d "$other" ] && [ ! -e "$other/disable" ] && [ ! -e "$other/remove" ]; then
      fail "存在冲突模块 $id，请先禁用它"
    fi
  done
}
for path in "$STATE" "$STATE/update.lock" "$STATE/supervisor.lock" "$STATE/core.lock"; do
  [ ! -L "$path" ] || fail '运行目录或锁文件异常，拒绝启动'
done
[ -d "$STATE" ] || fail '模块尚未完成安装，请重新安装并重启'
# Serialize manual launches with the installer, including the readiness wait.
exec 8>"$STATE/update.lock"
flock -n 8 || fail '安装更新或另一启动操作正在进行，请稍后重试'
guard
ready() { timeout 2 sh "$MODDIR/control.sh" upgrade-ready 8>&- 2>/dev/null; }
report=$(ready)
if printf '%s' "$report" | grep -q '"ready":true'; then
  [ ! -e "$STATE/stop" ] || fail '服务正在停止，请稍后重试'
  echo '{"ok":true,"state":"already_running"}'
  exit 0
fi
# Do not start alongside an unhealthy/stopping supervisor or orphan core.
exec 9>"$STATE/supervisor.lock"
flock -n 9 || fail '服务正在启动、停止或异常运行，请查看诊断后重试'
exec 7>"$STATE/core.lock"
flock -n 7 || fail '旧核心尚未退出，请稍后重试'
guard
exec 7>&- 9>&-
nohup setsid "$IAB_START_BB" sh "$MODDIR/service.sh" 8>&- 7>&- 9>&- \
  </dev/null >>"$STATE/supervisor.log" 2>&1 &
# Success requires the installed source identity and two advancing heartbeats.
ticks=0 first_beat=
while [ "$ticks" -lt 15 ]; do
  guard
  report=$(ready)
  if printf '%s' "$report" | grep -q '"ready":true'; then
    beat=$(printf '%s' "$report" | sed -n 's/.*"heartbeat_ms":\([0-9]*\).*/\1/p')
    if [ -n "$first_beat" ] && [ "$beat" -gt "$first_beat" ]; then
      echo '{"ok":true,"state":"started"}'
      exit 0
    fi
    first_beat=$beat
  else
    first_beat=
  fi
  sleep 1; ticks=$((ticks + 1))
done
fail '尚未确认服务就绪，请刷新状态并查看诊断和日志'
