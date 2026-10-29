#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
[ "$(getprop ro.build.version.sdk)" -ge 31 ] || abort 'Android 12 or newer is required (display brightness capabilities are checked at runtime).'
case "$ARCH" in
  arm64) ABI=arm64-v8a ;;
  arm) ABI=armeabi-v7a ;;
  x64) ABI=x86_64 ;;
  x86) ABI=x86 ;;
  *) abort "Unsupported architecture: $ARCH" ;;
esac
cp "$MODPATH/bin/$ABI/ios_brightness_daemon" "$MODPATH/bin/ios_brightness_daemon" || abort 'Missing native binary'
rm -rf "$MODPATH/bin/arm64-v8a" "$MODPATH/bin/armeabi-v7a" "$MODPATH/bin/x86_64" "$MODPATH/bin/x86"
STATE=/data/adb/ios_auto_brightness
[ ! -L "$STATE" ] || abort 'State directory must not be a symlink.'
mkdir -p "$STATE"
chmod 700 "$STATE"
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/bin/ios_brightness_daemon" 0 0 0755
for f in service.sh control.sh check_status.sh uninstall.sh hot-update.sh start-service.sh; do set_perm "$MODPATH/$f" 0 0 0755; done
# Keep reference keys from the existing or a v6.8 configuration, dropping keys
# of earlier rewrites; every kept value passes the core's own validation.
KEYS='alpha_fast|alpha_mid|alpha_slow|fast_threshold|slow_threshold|spike_up|spike_down|hyst_low|hyst_high|manual_gamma|high_lux_threshold|high_lux_boost_max|high_lux_min_pct|high_lux_duration_ms|learning_enabled|dual_sensor|log_max_bytes|log_max_backups'
DAEMON="$MODPATH/bin/ios_brightness_daemon"
OLD_CONFIG=
if [ -f "$STATE/config.conf" ]; then
  OLD_CONFIG="$STATE/config.conf"
elif [ -f /data/local/tmp/ios_brightness.conf ] && [ ! -L /data/local/tmp/ios_brightness.conf ]; then
  OLD_CONFIG=/data/local/tmp/ios_brightness.conf
fi
if [ -n "$OLD_CONFIG" ] && "$DAEMON" --validate-config < "$OLD_CONFIG" >/dev/null 2>&1; then
  [ "$OLD_CONFIG" = "$STATE/config.conf" ] || cp "$OLD_CONFIG" "$STATE/config.conf"
else
  MERGED="$STATE/.config-merge"
  cp "$MODPATH/default.conf" "$MERGED" || abort 'Cannot prepare configuration.'
  if [ -n "$OLD_CONFIG" ]; then
    grep -E "^($KEYS)=[-+.0-9eE]+$" "$OLD_CONFIG" | while IFS='=' read -r key value; do
      printf '%s=%s\n' "$key" "$value" | "$DAEMON" --validate-config >/dev/null 2>&1 || continue
      sed -i "s/^$key=.*/$key=$value/" "$MERGED"
    done
    "$DAEMON" --validate-config < "$MERGED" >/dev/null 2>&1 || cp "$MODPATH/default.conf" "$MERGED"
    ui_print 'Configuration migrated to the v6.8 reference keys.'
  fi
  mv -f "$MERGED" "$STATE/config.conf" || abort 'Cannot write configuration.'
fi
if [ ! -f "$STATE/learn.conf" ]; then
  for old in /data/local/tmp/ios_brightness_learn "$STATE/legacy-offset.dat"; do
    if [ -f "$old" ] && [ ! -L "$old" ]; then cp "$old" "$STATE/learn.conf"; break; fi
  done
fi
chmod 600 "$STATE"/*.conf 2>/dev/null
# Record first installation even if the hot-update helper cannot run. An active
# directory alone is not evidence of activation: managers may create it early.
ACTIVE=/data/adb/modules/ios_auto_brightness
BOOT_ID=$(cat /proc/sys/kernel/random/boot_id) || abort 'Cannot identify this boot.'
[ -n "$BOOT_ID" ] || abort 'Cannot identify this boot.'
if [ ! -f "$ACTIVE/control.sh" ] || [ ! -x "$ACTIVE/bin/ios_brightness_daemon" ]; then
  printf '%s\n' "$BOOT_ID" > "$STATE/first-install-boot" || abort 'Cannot record first installation.'
fi
sh "$MODPATH/hot-update.sh" prepare "$MODPATH" "$$" "$PPID" "$ZIPFILE" "$BOOTMODE"
hot_result=$?
case "$hot_result" in
  0) ui_print 'Hot update started successfully; WebUI becomes available after the installer exits.' ;;
  2) ui_print 'Installation staged. Reboot required to activate this module.' ;;
  *) abort 'Hot update failed. Inspect /data/adb/ios_auto_brightness/hot-update.log and updates/ backups.' ;;
esac
ui_print 'Existing learning offset and configuration are preserved.'
