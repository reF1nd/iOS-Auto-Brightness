#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
: "${ANDROID_HOME:?}" "${EMULATOR_API:?}"
export ANDROID_AVD_HOME="$PWD/build/avd"
mkdir -p "$ANDROID_AVD_HOME"
export PATH="$ANDROID_HOME/platform-tools:$ANDROID_HOME/emulator:$PATH"
image="system-images;android-$EMULATOR_API;google_apis;x86_64"
(set +o pipefail; yes | "$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" emulator "$image")
echo no | "$ANDROID_HOME/cmdline-tools/latest/bin/avdmanager" create avd --name iab --package "$image" --device pixel_2 --force
test -s "$ANDROID_AVD_HOME/iab.ini"
emulator -avd iab -no-window -no-audio -no-boot-anim -no-snapshot -gpu swiftshader_indirect > build/emulator.log 2>&1 &
cleanup() {
  timeout 10 adb shell cat /data/local/tmp/iab-state/bridge.log > build/smoke-bridge.log 2>/dev/null || true
  tail -60 build/emulator.log || true
  cat build/smoke-bridge.log || true
  timeout 10 adb emu kill || true
}
trap cleanup EXIT
timeout 120 adb wait-for-device
for _ in $(seq 1 120); do
  [[ "$(adb shell getprop sys.boot_completed | tr -d '\r')" != 1 ]] || break
  sleep 2
done
[[ "$(adb shell getprop sys.boot_completed | tr -d '\r')" == 1 ]]
adb root
timeout 120 adb wait-for-device
adb shell 'mkdir -p /data/local/tmp/iab-module/bin /data/local/tmp/iab-state; chmod 700 /data/local/tmp/iab-state'
adb push module/. /data/local/tmp/iab-module/ >/dev/null
adb push build/artifacts/core/ios_brightness_daemon /data/local/tmp/iab-module/bin/ >/dev/null
adb push build/artifacts/bridge/bridge.jar /data/local/tmp/iab-module/ >/dev/null
adb shell 'chmod 755 /data/local/tmp/iab-module/bin/ios_brightness_daemon; cp /data/local/tmp/iab-module/default.conf /data/local/tmp/iab-state/config.conf; settings put system screen_brightness_mode 1; input keyevent KEYCODE_WAKEUP'
adb shell 'CLASSPATH=/data/local/tmp/iab-module/bridge.jar nohup setsid app_process /system/bin org.ref1nd.brightness.Bridge /data/local/tmp/iab-module /data/local/tmp/iab-state --observe > /data/local/tmp/iab-state/bridge.log 2>&1 < /dev/null & sleep 2'
for _ in $(seq 1 15); do
  if adb shell cat /data/local/tmp/iab-state/status.json > build/smoke-status.json 2>/dev/null; then break; fi
  sleep 2
done
adb shell cat /data/local/tmp/iab-state/bridge.log > build/smoke-bridge.log
cat build/smoke-bridge.log
python3 - <<'PY'
import json
s=json.load(open('build/smoke-status.json'))
assert s.get('ok') and s.get('bridge')=='active' and s.get('observation_only'),s
assert s.get('mode')=='auto',s
assert s.get('calibration') and s.get('window_override_detection'),s
print({k:s.get(k) for k in ['bridge','profile','calibration','learning_available','window_override_detection','mode','reason']})
PY
sleep 6
adb shell cat /data/local/tmp/iab-state/status.json > build/smoke-status-next.json
python3 - <<'PY'
import json
a=json.load(open('build/smoke-status.json'));b=json.load(open('build/smoke-status-next.json'))
assert b.get('ok') and b.get('bridge')=='active' and b['heartbeat_ms']>a['heartbeat_ms'],b
assert b.get('window_override_detection') in ['framework','power_dump:0'],b
PY
# Inject only on the CI emulator, never on a user's device. Verify adapter wiring.
adb shell cmd thermalservice override-status 3
sleep 3
adb shell cat /data/local/tmp/iab-state/status.json > build/smoke-thermal.json
python3 - <<'PY'
import json
s=json.load(open('build/smoke-thermal.json'))
assert s.get('ok') and s.get('thermal_status')==3 and not s.get('apply'),s
assert s.get('protection') in ['system_thermal','system_brightness_limit'],s
assert not s.get('manual_learning_armed'),s
PY
adb shell cmd thermalservice reset
sleep 3
adb shell cat /data/local/tmp/iab-state/status.json > build/smoke-cooldown.json
python3 - <<'PY'
import json
s=json.load(open('build/smoke-cooldown.json'))
assert s.get('ok') and not s.get('apply') and s.get('protection')=='protection_cooldown',s
PY
sleep 12
adb shell cat /data/local/tmp/iab-state/status.json > build/smoke-resumed.json
python3 - <<'PY'
import json
s=json.load(open('build/smoke-resumed.json'))
assert s.get('ok') and s.get('protection')=='clear',s
assert s.get('core_pid',0)>0 and s.get('bridge_pid',0)>0 and s.get('boot_id'),s
assert s.get('cpu_core_percent',-1)>=0,s
PY
