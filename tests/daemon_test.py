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


def dumpsys(lux, logical, anchors):
    """dumpsys display: the AOSP default curve, the main display's thermal limit and the
    RefactorNitController of both displays."""
    text = ("  mConfig=BrightnessConfiguration{[(0.0, 1.0), (30.0, 83.13), (600.0, 126.14), (5000.0, 678.4)], {}, ''}\n\n"
            "Display Power Controller:\n-------------------------\n  mDisplayId=0\n"
            "  mCurrentThermalMaxBrightness=600.0\nmThermalMaxBrightness=0.5\n"
            " mBatteryThermalLimitBrightness=1.0\n\n"
            "Refactor Nit Controller Configuration:\n  mDisplayId=0\n  mIsHaveGoodCurve=false\n"
            f"  mUserLux={lux}\n  mUserLogicalNit={logical}\n"
            f"  anchors=[{anchors}]\n\n"
            "Refactor Nit Controller Configuration:\n  mDisplayId=1\n  mUserLux=5.0\n  mUserLogicalNit=9.0\n")
    return "#!/bin/sh\ncat <<'EOF'\n" + text + "EOF\n"


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
        write(state / 'config.conf', (ROOT / 'module/default.conf').read_text())
        write(state / 'learn.conf', 'user_offset=0.050000\nanchor=10.000 50.0000 1793000000\n')
        # The framework holds a dark drag from before the module started.
        dark = '(0.0,7.9595423,user), (30.0,135.0,def), (600.0,220.0,def), (5000.0,1060.0,def)'
        write(bin_dir / 'dumpsys', dumpsys(0.0, 7.9595423, dark))
        write(bin_dir / 'getprop', '#!/bin/sh\n')
        write(bin_dir / 'pm', '#!/bin/sh\n')
        write(bin_dir / 'cmd', '#!/bin/sh\ncase "$2" in lookup) echo true ;; list) echo com.miui.rom ;; esac\n')
        for tool in bin_dir.iterdir():
            tool.chmod(0o755)
        env = dict(os.environ, IAB_SYSROOT=str(root), PATH=f'{bin_dir}:{os.environ["PATH"]}')
        log = (base / 'daemon.log').open('w')
        prop = base / 'module.prop'
        shipped = (ROOT / 'module/module.prop').read_text()
        prop.write_text(shipped.replace('description=', 'description=[⏳ 重启后生效] '))

        def shown():
            line = next(line for line in prop.read_text().splitlines() if line.startswith('description='))
            state, _, rest = line[len('description=['):].partition('] ')
            assert 'description=' + rest in shipped.splitlines(), line
            return state
        process = subprocess.Popen([DAEMON, '--state', str(state), '--module-prop', str(prop)], env=env, stdout=log,
                                   stderr=subprocess.STDOUT)
        try:
            wait_for(lambda: (state / 'daemon.pid').exists(), 15, 'daemon did not start')
            first = status(process, state)
            assert first['ok'] and first['max'] == 16383, first
            assert first['lux_source'] == 'fallback' and first['settings'] == 'ok', first
            assert first['curve_source'] == 'RefactorNitController' and first['learnable'], first
            # The earlier learn.conf is not used; the framework's anchors are taken over.
            assert [a[:3] for a in first['anchors']] == [[0, 7.96, 1], [30, 135, 0], [600, 220, 0], [5000, 1060, 0]], first
            assert len(first['curve']) == 31 and first['curve'] != first['default_curve'], first
            assert 'anchor=0 7.959542' in (state / 'learn.conf').read_text()
            assert stat.S_IMODE((panel / 'brightness').stat().st_mode) == 0o444
            second = locked = None

            def advanced():
                nonlocal second
                second = status(process, state)
                return second['heartbeat_ms'] > first['heartbeat_ms'] and second['mode'] == 'auto'
            wait_for(advanced, 5, 'heartbeat did not advance')
            # The system's thermal limit from dumpsys; no dual sensor policy in this dump.
            limited = wait_for(lambda: (lambda s: s if s['thermal_limit'] == 0.5 else None)(status(process, state)), 5,
                               'thermal limit not read')
            assert not limited['dual_policy'] and limited['scene'] == 0, limited
            # The root manager's module list leads the description with the state.
            wait_for(lambda: shown() == '✅ 运行中 · 自动亮度 · dumpsys 轮询', 5, 'running state not shown')
            assert stat.S_IMODE(prop.stat().st_mode) == 0o644 and prop.read_text().count('\n') == shipped.count('\n')

            lux.write_text('3000.0\n')

            def followed():
                # The simulated node is a plain file: a shorter write leaves the old tail.
                current = int((panel / 'brightness').read_text())
                return current > 3000 and status(process, state)['current'] == current and current
            locked = wait_for(followed, 30, 'brightness did not follow brighter light')
            third = status(process, state)
            assert third['current'] == locked, (third, locked)

            other = subprocess.Popen([DAEMON, '--state', str(state)], env=env, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True)
            assert other.wait(10) == 1 and 'core.lock' in other.stdout.read()

            # A drag: RefactorNitController learns it, the framework persists
            # the slider and shows the chosen level on the panel.
            wait_for(lambda: status(process, state)['poll_ms'] >= 1600, 30, 'brightness did not settle')
            chosen = int((panel / 'brightness').read_text()) + 1500
            write(bin_dir / 'dumpsys', dumpsys(3000.0, 500.0, dark))
            (panel / 'brightness').chmod(0o644)
            write(panel / 'brightness', f'{chosen}\n')
            (panel / 'brightness').chmod(0o444)
            write(settings, settings_xml(1, 120))
            wait_for(lambda: 'anchor=3000 500 2 1\n' in (state / 'learn.conf').read_text(), 15, 'drag not learned')
            learned = status(process, state)
            assert learned['last_learning'] == 'learned', learned
            assert learned['user_lux'] == 3000.0 and learned['user_logical'] == 500.0, learned
            dragged = [a for a in learned['anchors'] if a[2]]
            assert [a[:2] for a in dragged] == [[0, 7.96], [3000.0, 500.0]], learned
            assert learned['curve'] != learned['default_curve'], learned
            # The dragged level stays until the panel settles, then the curve
            # through the anchor (x0.95 between 18:00 and 06:00) takes over.
            assert int((panel / 'brightness').read_text()) == chosen
            night = learned['local_hour'] >= 18 or learned['local_hour'] < 6
            expected = int(dragged[1][3] * (0.95 if night else 1.0))

            def through_anchor():
                now = status(process, state)
                # The smoothed lux is still creeping up to 3000 at the slow rate.
                return abs(now['target'] - expected) <= expected * 0.08 and now['current'] == now['target'] and now
            wait_for(through_anchor, 30, 'curve did not follow the learned anchor')

            process.send_signal(signal.SIGUSR1)
            wait_for(lambda: 'order=0\nuser=-1 -1\n' in (state / 'learn.conf').read_text(), 5,
                     'learning reset not saved')

            settings.write_text(settings_xml(0, 100))
            wait_for(lambda: stat.S_IMODE((panel / 'brightness').stat().st_mode) == 0o644, 15,
                     'manual mode did not release the node')
            assert status(process, state)['mode'] == 'manual'
            wait_for(lambda: shown() == '✅ 运行中 · 手动亮度，不接管', 5, 'manual state not shown')
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
        for needle in ('Fallback(sysfs)', 'Mode: AUTO', 'Mode: MANUAL', 'Learn: reset', 'Stopped',
                       'Learn: learn.conf of an older format is not used', 'Learn: took over 1 dragged anchors from the system',
                       'framework-ext-res.apk: cannot open; using the altdb values',
                       'Curve check: defaults vs BrightnessConfiguration max 0.2% at 30 lux',
                       'Learn: 3000.0 lux -> 500.00 logical nits'):
            assert needle in text, (needle, text)
    print('daemon_end_to_end: ok')


if __name__ == '__main__':
    main()
