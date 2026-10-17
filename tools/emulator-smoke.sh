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
  timeout 10 adb shell dumpsys display > build/smoke-display.log 2>/dev/null || true
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
adb shell 'chmod 755 /data/local/tmp/iab-module/bin/ios_brightness_daemon; cp /data/local/tmp/iab-module/default.conf /data/local/tmp/iab-state/config.conf; settings put system screen_brightness_mode 1; settings put system screen_off_timeout 600000; input keyevent KEYCODE_WAKEUP'
# Exercise startup/exit recovery even in observation CI: app_process is not an
# AMS-registered app and must use the external settings provider API.
adb shell 'CLASSPATH=/data/local/tmp/iab-module/bridge.jar app_process /system/bin org.ref1nd.brightness.Bridge release'
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
assert s.get('brightness_listener')=='brightness_events',s
assert 'interaction_guard' in s,s
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
adb shell 'CLASSPATH=/data/local/tmp/iab-module/bridge.jar app_process /system/bin org.ref1nd.brightness.Bridge health /data/local/tmp/iab-state /data/local/tmp/iab-module' > build/smoke-health.json
python3 - <<'PY'
import json
s=json.load(open('build/smoke-health.json'))
assert s.get('heartbeat')=='fresh' and s.get('bridge_process')=='alive' and s.get('core_process')=='alive',s
# No supervisor in this CI observation launch; healthy must not be falsely reported.
assert s.get('health')=='attention_required',s
PY

# Reference policy learns slider observations directly, without the old arm window.
adb emu sensor set light 100
stable=0
for _ in $(seq 1 30); do
  adb shell cat /data/local/tmp/iab-state/status.json > build/smoke-learning-ready.json
  if python3 - <<'PY'
import json,sys
s=json.load(open('build/smoke-learning-ready.json'))
sys.exit(0 if s.get('apply') and abs(s.get('lux',-1)-100)<.5 else 1)
PY
  then stable=1; break; fi
  sleep 2
done
[[ "$stable" == 1 ]]
adb shell settings put system screen_brightness 171
sleep 18
adb shell cat /data/local/tmp/iab-state/status.json > build/smoke-learned.json
python3 - <<'PYCODE'
import json
s=json.load(open('build/smoke-learned.json'))
assert s.get('ok') and s.get('storage')=='saved_reference_curve' and s.get('points'),s
assert s.get('control_policy')=='v68' and s.get('learning_model')=='bounded_curve_offset',s
assert any(abs(p['lux']-100)<1 for p in s['points']),s
assert not s.get('manual_learning_armed'),s
PYCODE

# Another slider adjustment updates the observation without opting into the old
# compatibility mode. Its reconstructed curve must change as well.
adb shell settings put system screen_brightness 181
sleep 12
adb shell cat /data/local/tmp/iab-state/status.json > build/smoke-compat-learned.json
python3 - <<'PYCODE'
import json
before=json.load(open('build/smoke-learned.json'));after=json.load(open('build/smoke-compat-learned.json'))
assert not after.get('settings_learning') and after.get('storage')=='saved_reference_curve',after
assert max(p['time'] for p in after['points'])>max(p['time'] for p in before['points']),after
assert after.get('event_status','').startswith('saved'),after
assert after['curve']!=before['curve'],after
PYCODE
adb shell cmd thermalservice override-status 3
sleep 3
adb shell settings put system screen_brightness 191
sleep 5
adb shell cat /data/local/tmp/iab-state/status.json > build/smoke-compat-protected.json
python3 - <<'PYCODE'
import json
a=json.load(open('build/smoke-compat-learned.json'));b=json.load(open('build/smoke-compat-protected.json'))
assert b.get('points')==a.get('points') and not b.get('apply') and not b.get('manual_learning_armed'),b
assert b.get('protection')=='system_thermal',b
PYCODE
adb shell cmd thermalservice reset
adb shell 'CLASSPATH=/data/local/tmp/iab-module/bridge.jar app_process /system/bin org.ref1nd.brightness.Bridge backlight' > build/smoke-backlight.json
python3 - <<'PYCODE'
import json
s=json.load(open('build/smoke-backlight.json'))
assert s.get('ok') and isinstance(s.get('nodes'),list),s
PYCODE
