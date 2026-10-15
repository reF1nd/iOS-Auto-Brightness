#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Transactional replacement of this module only; all persistent user data stays put.
HOT_ACTIVE=/data/adb/modules/ios_auto_brightness
HOT_STAGED=/data/adb/modules_update/ios_auto_brightness
HOT_STATE=/data/adb/ios_auto_brightness
HOT_PROC=/proc
HOT_TX=
HOT_STOPPED=0 HOT_TRANSFERRED=0

hot_stamp() {
  case "$1" in ''|*[!0-9]*) return 1 ;; esac
  [ "$1" -gt 1 ] || return 1
  sed 's/^.*) //' "$HOT_PROC/$1/stat" 2>/dev/null | awk '$1 != "Z" && NF >= 20 {print $20}'
}
hot_parent() { sed 's/^.*) //' "$HOT_PROC/$1/stat" 2>/dev/null | awk 'NF >= 20 {print $2}'; }
hot_installer() {
  [ -n "$HOT_ZIP" ] || return 1
  tr '\000' '\n' < "$HOT_PROC/$1/cmdline" 2>/dev/null | awk -v zip="$HOT_ZIP" '
    {a[NR]=$0} END {for(i=2;i<=NR;i++) {
      if(a[i]=="module" && a[i+1]=="install" && a[i+2]==zip) exit 0;
      if(a[i]=="--install-module" && a[i+1]==zip) exit 0;
    } exit 1}'
}
hot_find_installer() {
  HOT_PARENT= HOT_PARENT_STAMP=
  hot_pid=$1 hot_depth=0
  while [ "$hot_pid" != 1 ]; do
    hot_stamp_before=$(hot_stamp "$hot_pid")
    [ -n "$hot_stamp_before" ] && [ "$hot_depth" -lt 16 ] || return 1
    hot_next=$(hot_parent "$hot_pid")
    if hot_installer "$hot_pid"; then HOT_PARENT=$hot_pid HOT_PARENT_STAMP=$hot_stamp_before; fi
    [ "$(hot_stamp "$hot_pid")" = "$hot_stamp_before" ] && [ "$hot_next" != "$hot_pid" ] || return 1
    hot_pid=$hot_next hot_depth=$((hot_depth + 1))
  done
  [ -n "$HOT_PARENT_STAMP" ]
}
hot_manifest() (
  cd "$1" || exit 1
  find . -type f ! -path ./update ! -path ./disable ! -path ./remove ! -path ./.iab-install-token | sort |
    while IFS= read -r hot_file; do sha256sum "$hot_file" || exit 1; done
)
hot_matches() {
  hot_manifest "$1" > "$HOT_TX/check.manifest" && cmp -s "$HOT_TX/check.manifest" "$2"
}
hot_flags() { [ -e "$HOT_ACTIVE/disable" ] || [ -e "$HOT_ACTIVE/remove" ]; }
hot_health() {
  timeout 4 sh "$HOT_ACTIVE/control.sh" health 2>/dev/null | grep -q '"health":"healthy"'
}
hot_classify() {
  hot_boot=$(cat "$HOT_PROC/sys/kernel/random/boot_id") || return 1
  [ -n "$hot_boot" ] || return 1
  # A pending first install MUST remain first-install until the boot ID changes,
  # even if a manager has already populated the active directory.
  if [ "$(cat "$HOT_STATE/first-install-boot" 2>/dev/null)" = "$hot_boot" ]; then return 2; fi
  if [ -f "$HOT_ACTIVE/bridge.jar" ] && [ -x "$HOT_ACTIVE/bin/ios_brightness_daemon" ] &&
     { [ -f "$HOT_STATE/activated-boot" ] || [ -f "$HOT_STATE/first-install-boot" ] || hot_health; }; then
    return 0
  fi
  printf '%s\n' "$hot_boot" > "$HOT_STATE/first-install-boot" || return 1
  return 2
}
hot_preflight() {
  [ "$HOT_PATH" = "$HOT_STAGED" ] && [ "$HOT_BOOTMODE" = true ] || return 1
  [ "$(getprop sys.boot_completed)" = 1 ] || return 1
  for hot_path in "$HOT_ACTIVE" "$HOT_STAGED" "${HOT_ACTIVE%/*}" "${HOT_STAGED%/*}" "$HOT_STATE" \
      "$HOT_STATE/update.lock" "$HOT_STATE/supervisor.lock" "$HOT_STATE/core.lock" "$HOT_STATE/updates"; do
    [ ! -L "$hot_path" ] || return 1
  done
  awk -v a="${HOT_ACTIVE%/*}" -v s="${HOT_STAGED%/*}" -v m="$HOT_ACTIVE" -v n="$HOT_STAGED" '
    $2==a || $2==s || $2==m || $2==n || index($2,m"/")==1 || index($2,n"/")==1 {bad=1}
    END {exit bad}' "$HOT_PROC/mounts" || return 1
  [ "$(stat -c %d "$HOT_STATE")" = "$(stat -c %d "${HOT_ACTIVE%/*}")" ] || return 1
  for hot_tree in "$HOT_ACTIVE" "$HOT_STAGED"; do
    [ "$(sed -n 's/^id=//p' "$hot_tree/module.prop")" = ios_auto_brightness ] || return 1
    [ -f "$hot_tree/skip_mount" ] || return 1
    [ -z "$(find "$hot_tree" ! -type f ! -type d -print)" ] || return 1
    for hot_hook in system vendor product system_ext odm system.prop sepolicy.rule post-fs-data.sh post-mount.sh boot-completed.sh; do
      [ ! -e "$hot_tree/$hot_hook" ] || return 1
    done
  done
  for hot_file in bridge.jar bin/ios_brightness_daemon service.sh control.sh hot-update.sh webroot/index.html SOURCE.txt; do
    [ -s "$HOT_STAGED/$hot_file" ] || return 1
  done
  hot_flags && return 1
  [ ! -e "$HOT_STATE/stop" ] && hot_health
}
hot_lock_service() {
  # Root-manager BusyBox flock has no util-linux -w option.
  hot_lock_ticks=0
  until flock -n "$1"; do
    [ "$hot_lock_ticks" -lt 15 ] || return 1
    sleep 1; hot_lock_ticks=$((hot_lock_ticks + 1))
  done
}
hot_stop() {
  # The socket asks the bridge to run its shutdown hook. The supervisor also
  # observes stop. Locks prove BOTH supervisor cleanup and native exit completed.
  touch "$HOT_STATE/stop" || return 1
  timeout 8 sh "$HOT_ACTIVE/control.sh" stop || :
  exec 9>"$HOT_STATE/supervisor.lock"
  hot_lock_service 9 || return 1
  exec 7>"$HOT_STATE/core.lock"
  hot_lock_service 7 || return 1
}
hot_unlock_service() { exec 7>&- 9>&-; }
hot_start() {
  hot_unlock_service
  hot_flags && return 1
  # In particular, do not let the service inherit the update transaction lock.
  nohup setsid "$IAB_BB" sh "$HOT_ACTIVE/service.sh" 8>&- 7>&- 9>&- \
    </dev/null >>"$HOT_STATE/supervisor.log" 2>&1 &
}
hot_ready() {
  hot_ticks=0 hot_first=
  while [ "$hot_ticks" -lt 25 ]; do
    hot_flags && return 1
    hot_report=$(timeout 4 sh "$HOT_ACTIVE/control.sh" upgrade-ready 2>/dev/null)
    if printf '%s' "$hot_report" | grep -q '"ready":true'; then
      hot_beat=$(printf '%s' "$hot_report" | sed -n 's/.*"heartbeat_ms":\([0-9]*\).*/\1/p')
      if [ -n "$hot_first" ] && [ "$hot_beat" -gt "$hot_first" ]; then return 0; fi
      hot_first=$hot_beat
    else
      hot_first=
    fi
    sleep 1; hot_ticks=$((hot_ticks + 1))
  done
  return 1
}
hot_restore() {
  # Filesystem journal: old/ exists immediately after the first atomic rename,
  # even if a signal arrived before the shell could update an in-memory flag.
  if [ -d "$HOT_TX/old" ]; then
    if [ -d "$HOT_ACTIVE" ]; then
      hot_stop || { echo 'Rollback blocked: new service did not stop; backup retained'; return 1; }
      for hot_flag in disable remove; do [ ! -e "$HOT_ACTIVE/$hot_flag" ] || touch "$HOT_TX/old/$hot_flag"; done
      mv "$HOT_ACTIVE" "$HOT_TX/failed-active" || return 1
    fi
    mv "$HOT_TX/old" "$HOT_ACTIVE" || return 1
  fi
  # Do not let a rejected new version activate on the next boot. Only quarantine
  # the exact staged transaction we own; leave concurrent installer files alone.
  if [ "$(cat "$HOT_STAGED/.iab-install-token" 2>/dev/null)" = "$HOT_TOKEN" ] &&
     hot_matches "$HOT_STAGED" "$HOT_TX/new.manifest"; then
    mv "$HOT_STAGED" "$HOT_TX/failed-stage" || return 1
  fi
  hot_unlock_service
  if [ "$HOT_STOPPED" = 1 ] && ! hot_flags; then
    hot_start || return 1
    hot_retry=0
    until hot_health; do
      [ "$hot_retry" -lt 20 ] || { echo 'Old module restored but service health unconfirmed'; return 1; }
      sleep 1; hot_retry=$((hot_retry + 1))
    done
  fi
  printf 'rolled_back\n' > "$HOT_STATE/update-status"
  echo "Old module restored; persistent settings/learning untouched. Backup: $HOT_TX"
}
hot_cleanup() {
  [ "$HOT_TRANSFERRED" = 1 ] && return
  if [ "$HOT_STOPPED" = 1 ]; then hot_restore >>"$HOT_STATE/hot-update.log" 2>&1; fi
  hot_unlock_service
}
hot_finish() {
  hot_wait=0
  while [ "$(hot_stamp "$HOT_SHELL")" = "$HOT_SHELL_STAMP" ] || [ "$(hot_stamp "$HOT_PARENT")" = "$HOT_PARENT_STAMP" ]; do
    [ "$hot_wait" -lt 90 ] || { echo 'Installer exit unconfirmed; keeping pending update'; return 1; }
    sleep 1; hot_wait=$((hot_wait + 1))
  done
  [ "$(cat "$HOT_STAGED/.iab-install-token" 2>/dev/null)" = "$HOT_TOKEN" ] || return 1
  hot_matches "$HOT_STAGED" "$HOT_TX/new.manifest" || return 1
  hot_matches "$HOT_ACTIVE" "$HOT_TX/new.manifest" || return 1
  hot_ready || return 1
  # The native installer may recreate update after customize.sh returns.
  # Retain its staged copy for the manager's normal mount/next-boot lifecycle.
  rm -f "$HOT_ACTIVE/update" || return 1
  printf 'ready\n' > "$HOT_STATE/update-status"
  echo 'Hot update complete. WebUI ready without reboot.'
}
hot_prepare() {
  HOT_PATH=$1 HOT_SHELL=$2 HOT_ZIP=$4 HOT_BOOTMODE=$5
  umask 077
  [ ! -L "$HOT_STATE" ] && [ ! -L "$HOT_STATE/update.lock" ] || return 1
  mkdir -p "$HOT_STATE" || return 1
  exec 8>"$HOT_STATE/update.lock"
  flock -n 8 || { echo 'Another update is in progress'; return 1; }
  hot_classify; hot_kind=$?
  if [ "$hot_kind" = 2 ]; then echo 'First install (including reinstall before reboot): reboot required.'; return 2; fi
  [ "$hot_kind" = 0 ] || return 1
  hot_preflight || { echo 'Update staged: live activation unavailable or module stopped/disabled.'; return 2; }
  HOT_SHELL_STAMP=$(hot_stamp "$HOT_SHELL")
  [ -n "$HOT_SHELL_STAMP" ] && hot_find_installer "$3" || { echo 'Installer identity unknown; reboot activation retained.'; return 2; }
  mkdir -p "$HOT_STATE/updates" || return 1
  HOT_TX=$(mktemp -d "$HOT_STATE/updates/txn.XXXXXX") || return 1
  HOT_TOKEN=${HOT_TX##*/}
  trap hot_cleanup EXIT
  trap 'exit 1' HUP INT TERM
  cp "$HOT_STAGED/hot-update.sh" "$HOT_TX/worker.sh" || return 1
  mkdir "$HOT_TX/new" "$HOT_TX/data-backup" || return 1
  cp -a "$HOT_STAGED/." "$HOT_TX/new/" || return 1
  rm -f "$HOT_TX/new/update" "$HOT_TX/new/.iab-install-token"
  hot_manifest "$HOT_TX/new" > "$HOT_TX/new.manifest" || return 1
  printf '%s\n' "$HOT_TOKEN" > "$HOT_STAGED/.iab-install-token" || return 1
  hot_matches "$HOT_STAGED" "$HOT_TX/new.manifest" || return 1
  printf 'stopping\n' > "$HOT_STATE/update-status"
  HOT_STOPPED=1
  hot_stop || return 1
  for hot_data in "$HOT_STATE"/*.conf "$HOT_STATE"/*.dat "$HOT_STATE"/*.dat.bak; do
    [ ! -f "$hot_data" ] || cp -p "$hot_data" "$HOT_TX/data-backup/" || return 1
  done
  hot_flags && return 1
  hot_matches "$HOT_STAGED" "$HOT_TX/new.manifest" || return 1
  mv "$HOT_ACTIVE" "$HOT_TX/old" || return 1
  mv "$HOT_TX/new" "$HOT_ACTIVE" || return 1
  for hot_flag in disable remove; do [ ! -e "$HOT_TX/old/$hot_flag" ] || touch "$HOT_ACTIVE/$hot_flag"; done
  touch "$HOT_ACTIVE/update" || return 1
  printf 'starting\n' > "$HOT_STATE/update-status"
  hot_start && hot_ready || return 1
  printf 'waiting_for_installer\n' > "$HOT_STATE/update-status"
  nohup setsid "$IAB_BB" sh "$HOT_TX/worker.sh" finish "$HOT_TX" "$HOT_TOKEN" \
    "$HOT_SHELL" "$HOT_SHELL_STAMP" "$HOT_PARENT" "$HOT_PARENT_STAMP" \
    </dev/null >>"$HOT_STATE/hot-update.log" 2>&1 &
  hot_worker=$! hot_wait=0
  while [ ! -f "$HOT_TX/worker-ready" ]; do
    kill -0 "$hot_worker" 2>/dev/null && [ "$hot_wait" -lt 5 ] || return 1
    sleep 1; hot_wait=$((hot_wait + 1))
  done
  HOT_TRANSFERRED=1
  echo 'New service healthy; WebUI unlock follows installer completion (no reboot).'
}

[ "$1" != --library ] || return 0
if [ "${IAB_UPDATE_SHELL_READY:-}" != "$$" ]; then
  export IAB_UPDATE_SHELL_READY=$$ ASH_STANDALONE=1
  for IAB_BB in /data/adb/ksu/bin/busybox /data/adb/ap/bin/busybox /data/adb/magisk/busybox; do
    if [ -x "$IAB_BB" ]; then export IAB_BB; exec "$IAB_BB" sh "$0" "$@"; fi
  done
  echo 'Root manager BusyBox required; normal staged installation retained.'; exit 2
fi
case "$1" in
  prepare) shift; hot_prepare "$@"; exit $? ;;
  finish)
    HOT_TX=$2 HOT_TOKEN=$3 HOT_SHELL=$4 HOT_SHELL_STAMP=$5 HOT_PARENT=$6 HOT_PARENT_STAMP=$7
    case "$HOT_TX" in "$HOT_STATE/updates/txn."*) ;; *) exit 1 ;; esac
    [ -d "$HOT_TX" ] && [ ! -L "$HOT_TX" ] && [ "${HOT_TX##*/}" = "$HOT_TOKEN" ] || exit 1
    : > "$HOT_TX/worker-ready"
    if hot_finish; then exit 0; else echo 'Pending update retained; see transaction backup.'; printf 'pending\n' > "$HOT_STATE/update-status"; exit 1; fi ;;
  *) exit 1 ;;
esac
