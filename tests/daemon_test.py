#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run the real daemon against a simulated sysfs/settings tree (IAB_SYSROOT)."""
import json
import os
import signal
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path

DAEMON = Path(sys.argv[1]).resolve()
ROOT = Path(__file__).resolve().parent.parent


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def settings_xml(mode, brightness):
    return ("<?xml version='1.0' encoding='utf-8'?>\n<settings version=\"236\">\n"
            f"<setting id=\"1\" name=\"screen_brightness_mode\" value=\"{mode}\" package=\"android\" />\n"
            f"<setting id=\"2\" name=\"screen_brightness\" value=\"{brightness}\" package=\"android\" />\n"
            "</settings>\n")


def wait_for(predicate, timeout, message):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.1)
    raise AssertionError(message)


def status(process, state):
    path = state / 'status.json'
    before = path.stat().st_mtime_ns if path.exists() else 0
    process.send_signal(signal.SIGUSR2)

    def fresh():
        if path.exists() and path.stat().st_mtime_ns != before:
            return json.loads(path.read_text())
        return None
    return wait_for(fresh, 3, 'status.json not refreshed')


def validate(text):
    result = subprocess.run([DAEMON, '--validate-config'], input=text, text=True, capture_output=True)
    return result.returncode, json.loads(result.stdout)


def main():
    code, out = validate((ROOT / 'module/default.conf').read_text())
    assert code == 0 and out['ok'], out
    assert validate('alpha_fast=2\n')[0] == 1
    assert validate('unknown=1\n')[0] == 1
    assert validate('fast_threshold=0.01\nslow_threshold=0.05\n')[0] == 1
    assert validate('spike_up=abc\n')[0] == 1

    with tempfile.TemporaryDirectory() as temporary:
        base = Path(temporary)
        root, state, bin_dir = base / 'root', base / 'state', base / 'bin'
        state.mkdir()
        panel = root / 'sys/class/backlight/panel0-backlight'
        write(panel / 'brightness', '1000\n')
        write(panel / 'max_brightness', '16383\n')
        write(panel / 'bl_power', '0\n')
        lux = root / 'sys/bus/iio/devices/iio:device0/in_illuminance_input'
        write(lux, '50.0\n')
        settings = root / 'data/system/users/0/settings_system.xml'
        write(settings, settings_xml(1, 100))
        write(root / 'sys/class/power_supply/battery/capacity', '80\n')
        write(root / 'sys/class/power_supply/battery/status', 'Discharging\n')
        write(root / 'sys/class/thermal/thermal_zone0/type', 'battery\n')
        write(root / 'sys/class/thermal/thermal_zone0/temp', '30000\n')
        write(state / 'config.conf', (ROOT / 'module/default.conf').read_text())
        write(state / 'learn.conf', 'user_offset=0.050000\n')
        write(bin_dir / 'dumpsys', '#!/bin/sh\necho "    mUserLux=-1.0"\necho "    mUserBrightness=NaN"\n')
        write(bin_dir / 'getprop', '#!/bin/sh\n')
        for tool in bin_dir.iterdir():
            tool.chmod(0o755)
        env = dict(os.environ, IAB_SYSROOT=str(root), PATH=f'{bin_dir}:{os.environ["PATH"]}')
        log = (base / 'daemon.log').open('w')
        process = subprocess.Popen([DAEMON, '--state', str(state)], env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            wait_for(lambda: (state / 'daemon.pid').exists(), 15, 'daemon did not start')
            first = status(process, state)
            assert first['ok'] and first['max'] == 16383, first
            assert first['lux_source'] == 'fallback' and first['settings'] == 'ok', first
            assert abs(first['user_offset'] - 0.05) < 1e-6, first
            assert stat.S_IMODE((panel / 'brightness').stat().st_mode) == 0o444
            second = locked = None

            def advanced():
                nonlocal second
                second = status(process, state)
                return second['heartbeat_ms'] > first['heartbeat_ms'] and second['mode'] == 'auto'
            wait_for(advanced, 5, 'heartbeat did not advance')

            lux.write_text('3000.0\n')

            def followed():
                current = int((panel / 'brightness').read_text())
                return current > 6000 and current
            locked = wait_for(followed, 30, 'brightness did not follow brighter light')
            third = status(process, state)
            assert third['current'] == locked, (third, locked)

            other = subprocess.Popen([DAEMON, '--state', str(state)], env=env, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True)
            assert other.wait(10) == 1 and 'core.lock' in other.stdout.read()

            process.send_signal(signal.SIGUSR1)
            wait_for(lambda: 'user_offset=0.000000' in (state / 'learn.conf').read_text(), 5,
                     'learning reset not saved')

            settings.write_text(settings_xml(0, 100))
            wait_for(lambda: stat.S_IMODE((panel / 'brightness').stat().st_mode) == 0o644, 15,
                     'manual mode did not release the node')
            assert status(process, state)['mode'] == 'manual'
            settings.write_text(settings_xml(1, 100))
            wait_for(lambda: stat.S_IMODE((panel / 'brightness').stat().st_mode) == 0o444, 15,
                     'auto mode did not lock the node again')

            process.send_signal(signal.SIGTERM)
            assert process.wait(10) == 0
            assert stat.S_IMODE((panel / 'brightness').stat().st_mode) == 0o644
            assert not (state / 'daemon.pid').exists() and not (state / 'status.json').exists()
            assert (state / 'backlight').read_text().strip() == str(panel)
        except BaseException:
            log.flush()
            print((base / 'daemon.log').read_text(), file=sys.stderr)
            raise
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            log.close()
        text = (base / 'daemon.log').read_text()
        for needle in ('Fallback(sysfs)', 'Mode: AUTO', 'Mode: MANUAL', 'Learn: offset reset', 'Stopped'):
            assert needle in text, (needle, text)
    print('daemon_end_to_end: ok')


if __name__ == '__main__':
    main()
