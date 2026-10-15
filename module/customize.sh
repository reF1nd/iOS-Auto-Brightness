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
# Preserve an existing v7 config. Legacy offset migration never invents samples.
if [ ! -f "$STATE/config.conf" ]; then
  if [ -f /data/local/tmp/ios_brightness.conf ] && [ ! -L /data/local/tmp/ios_brightness.conf ]; then
    cp /data/local/tmp/ios_brightness.conf "$STATE/legacy-config.conf"
    ui_print 'Legacy tuning will be validated and imported by the core at startup.'
  else
    cp "$MODPATH/default.conf" "$STATE/config.conf"
  fi
fi
if [ ! -e "$STATE/legacy-offset.dat" ] && [ -f /data/local/tmp/ios_brightness_learn ] && [ ! -L /data/local/tmp/ios_brightness_learn ]; then
  cp /data/local/tmp/ios_brightness_learn "$STATE/legacy-offset.dat"
fi
chmod 600 "$STATE"/*.conf "$STATE"/*.dat 2>/dev/null
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/bin/ios_brightness_daemon" 0 0 0755
for f in service.sh control.sh check_status.sh uninstall.sh; do set_perm "$MODPATH/$f" 0 0 0755; done
ui_print 'iOS Auto Brightness v7: reboot to replace the previous service.'
ui_print 'Existing learning data and configuration are preserved.'
