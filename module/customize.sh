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
for f in service.sh control.sh check_status.sh uninstall.sh start-service.sh hotinstall.sh hot-update.sh; do set_perm "$MODPATH/$f" 0 0 0755; done
# Keep the known keys from the existing or an older configuration, dropping keys
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
    ui_print 'Configuration migrated to the current keys.'
  fi
  mv -f "$MERGED" "$STATE/config.conf" || abort 'Cannot write configuration.'
fi
chmod 600 "$STATE"/*.conf 2>/dev/null
# The active tree belongs to this boot; modules_update may already contain a
# different pending ZIP. Compare the installed payload before requesting a swap.
unset MODULE_HOT_INSTALL_REQUEST MODULE_HOT_RUN_SCRIPT
if sh "$MODPATH/hot-update.sh" eligible "$MODPATH" "${BOOTMODE:-}"; then
  if [ "${KSU:-}" = true ] && [ "${KSU_HAS_HOT_INSTALL:-}" = true ]; then
    export MODULE_HOT_INSTALL_REQUEST=true MODULE_HOT_RUN_SCRIPT=hotinstall.sh
    ui_print 'KernelSU will activate this update when installation finishes.'
  else
    sh "$MODPATH/hot-update.sh" prepare "$MODPATH" "$$" "$PPID" "$ZIPFILE" "$BOOTMODE"
    case "$?" in
      0) ui_print 'Module updated for this boot.' ;;
      2) ui_print 'Update staged for the next boot.' ;;
      *) abort 'Hot update failed; see /data/adb/ios_auto_brightness/hot-update.log.' ;;
    esac
  fi
else
  case "$?" in
    2) ;;
    *) abort 'Cannot determine module activation state.' ;;
  esac
  rm -f "$STATE/zygisk.disabled" "$STATE/zygisk.starts"
  sed -i 's/^description=/description=[⏳ 重启后生效] /' "$MODPATH/module.prop"
  ui_print 'Reboot required for first activation or changed Zygisk payload.'
fi
