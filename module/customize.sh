#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
[ "$KSU" = true ] || abort 'KernelSU is required for this build.'
[ "$ARCH" = arm64 ] || abort 'This build requires ARM64.'
[ "$(getprop ro.build.version.sdk)" -ge 37 ] || abort 'This build targets the Android 17 HyperOS display adapter.'
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
