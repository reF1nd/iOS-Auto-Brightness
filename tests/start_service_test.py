#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""CI-only launcher integration: real shell, locks and detached service fixtures."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import time
import unittest

BUSYBOX = os.environ.get('HOT_TEST_BUSYBOX')
SHELL = [BUSYBOX, 'sh'] if BUSYBOX else ['bash']


class StartServiceTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.mod = self.root / 'modules/ios_auto_brightness'
        self.state = self.root / 'state'
        self.mod.mkdir(parents=True)
        self.state.mkdir()
        self.boot = self.root / 'boot_id'
        self.boot.write_text('boot-A\n')
        (self.state / 'activated-boot').write_text('boot-A\n')
        (self.mod / 'bin').mkdir()
        for name in ['bridge.jar', 'SOURCE.txt', 'bin/ios_brightness_daemon']:
            (self.mod / name).write_text('fixture')
            (self.mod / name).chmod(0o755)
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        (self.bin / 'getprop').write_text('#!/bin/sh\necho 1\n')
        (self.bin / 'getprop').chmod(0o755)
        self.bb = self.bin / 'busybox'
        self.bb.write_text('#!/bin/sh\n' + (f'exec {shlex.quote(BUSYBOX)} "$@"\n' if BUSYBOX else 'shift\nexec bash "$@"\n'))
        self.bb.chmod(0o755)
        source = Path('module/start-service.sh').read_text()
        source = source.replace('if [ "${IAB_START_SHELL_READY:-}" != "$$" ]; then', 'if false; then')
        source = source.replace('/data/adb/modules', str(self.mod.parent)).replace('/data/adb/ios_auto_brightness', str(self.state))
        source = source.replace('/proc/sys/kernel/random/boot_id', str(self.boot)).replace('sleep 1;', 'sleep 0.05;')
        self.script = self.mod / 'start-service.sh'
        self.script.write_text(source)
        (self.mod / 'process-group.sh').write_text('iab_detach_cgroups() { [ ! -e "' + str(self.state / 'detach-fail') + '" ]; }\n')
        self.env = dict(os.environ, PATH=f'{self.bin}:{os.environ["PATH"]}', IAB_START_BB=str(self.bb))
        self.write_service()
        self.write_health()
        self.children = []

    def write_service(self):
        (self.mod / 'service.sh').write_text(f'''#!/bin/sh
state={shlex.quote(str(self.state))}
mod={shlex.quote(str(self.mod))}
exec 9>"$state/supervisor.lock"
flock -n 9 || exit 0
exec 7>"$state/core.lock"
flock -n 7 || exit 1
echo $$ > "$state/mock.pid"
echo started >> "$state/starts"
rm -f "$state/stop"
trap 'rm -f "$state/mock.pid" "$state/ready"; exit 0' EXIT INT TERM
touch "$state/ready"
while [ ! -f "$mod/disable" ] && [ ! -f "$mod/remove" ] && [ ! -f "$state/stop" ]; do sleep 0.02; done
''')

    def write_health(self, stale=False):
        (self.mod / 'control.sh').write_text(f'''#!/bin/sh
state={shlex.quote(str(self.state))}
if [ -f "$state/ready" ]; then
  beat=$(cat "$state/beat" 2>/dev/null || echo 0)
  beat=$((beat + {0 if stale else 1}))
  echo "$beat" > "$state/beat"
  printf '{{"ready":true,"heartbeat_ms":%s}}\\n' "$beat"
else echo '{{"ready":false}}'; fi
''')

    def launch(self):
        p = subprocess.Popen(SHELL + [str(self.script)], env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.children.append(p)
        return p

    def result(self, p=None):
        p = p or self.launch()
        out, err = p.communicate(timeout=8)
        self.assertEqual(p.returncode, 0, err)
        return json.loads(out)

    def wait_stopped(self):
        for _ in range(150):
            if not (self.state / 'mock.pid').exists():
                return
            time.sleep(.02)
        self.fail('fixture service did not stop')

    def tearDown(self):
        for child in self.children:
            if child.poll() is None:
                child.terminate()
                child.communicate(timeout=3)
        (self.state / 'stop').touch()
        self.wait_stopped()
        self.temp.cleanup()

    def test_start_idempotent_disable_and_explicit_restart(self):
        (self.state / 'stop').touch()
        self.assertEqual(self.result()['state'], 'started')
        self.assertEqual(self.result()['state'], 'already_running')
        self.assertEqual((self.state / 'starts').read_text().count('started'), 1)
        (self.mod / 'disable').touch()
        self.wait_stopped()
        self.assertFalse(self.result()['ok'])
        (self.mod / 'disable').unlink()
        time.sleep(.1)
        self.assertFalse((self.state / 'ready').exists(), 'no resident automatic restart')
        self.assertEqual(self.result()['state'], 'started')
        self.assertEqual((self.state / 'starts').read_text().count('started'), 2)

    def test_cgroup_failure_never_spawns_service(self):
        (self.state / 'output-fault').write_text('driver_output_mismatch\n')
        (self.state / 'detach-fail').touch()
        result = self.result()
        self.assertFalse(result['ok'])
        self.assertIn('进程回收组', result['error'])
        self.assertFalse((self.state / 'starts').exists())
        self.assertTrue((self.state / 'output-fault').exists())

    def test_only_explicit_stopped_start_clears_output_fault(self):
        fault = self.state / 'output-fault'
        fault.write_text('driver_output_mismatch\n')
        (self.mod / 'disable').touch()
        self.assertFalse(self.result()['ok'])
        self.assertTrue(fault.exists())
        (self.mod / 'disable').unlink()
        self.assertEqual(self.result()['state'], 'started')
        self.assertFalse(fault.exists())
        fault.write_text('output_probe_timeout\n')
        self.assertEqual(self.result()['state'], 'already_running')
        self.assertTrue(fault.exists(), 'a live service must first be stopped')

    def test_guards_preserve_stop_and_data(self):
        for file in ('config.conf', 'learning.dat'):
            (self.state / file).write_text('preserve')
        for flag in ('disable', 'remove', 'update'):
            with self.subTest(flag=flag):
                (self.mod / flag).touch()
                (self.state / 'stop').touch()
                self.assertFalse(self.result()['ok'])
                self.assertTrue((self.state / 'stop').exists())
                self.assertFalse((self.state / 'starts').exists())
                (self.mod / flag).unlink()
        for file in ('config.conf', 'learning.dat'):
            self.assertEqual((self.state / file).read_text(), 'preserve')

    def test_first_install_reinstall_and_boot_disabled(self):
        (self.state / 'first-install-boot').write_text('boot-A\n')
        # Even a populated active directory and activation marker cannot bypass it.
        self.assertFalse(self.result()['ok'])
        self.assertFalse(self.result()['ok'])
        self.boot.write_text('boot-B\n')
        self.assertTrue(self.result()['ok'])

    def test_unknown_install_and_conflict(self):
        (self.state / 'activated-boot').unlink()
        self.assertFalse(self.result()['ok'])
        (self.state / 'activated-boot').write_text('boot-A\n')
        other = self.mod.parent / 'autobrightness'
        other.mkdir()
        self.assertIn('冲突', self.result()['error'])
        self.assertFalse((self.state / 'starts').exists())

    def test_concurrent_clicks(self):
        results = [self.result(p) for p in [self.launch(), self.launch()]]
        self.assertTrue(any(r.get('ok') for r in results))
        self.assertEqual((self.state / 'starts').read_text().count('started'), 1)

    def test_update_and_orphan_locks(self):
        for lock in ('update.lock', 'supervisor.lock', 'core.lock'):
            with self.subTest(lock=lock):
                holder = subprocess.Popen(SHELL + ['-c', 'exec 9>"$1"; flock 9; echo locked; read -r line', 'fixture', str(self.state / lock)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
                self.assertEqual(holder.stdout.readline().strip(), 'locked')
                try:
                    self.assertFalse(self.result()['ok'])
                    self.assertFalse((self.state / 'starts').exists())
                finally:
                    holder.communicate('\n', timeout=3)

    def test_stale_heartbeat_not_success(self):
        self.write_health(stale=True)
        self.assertFalse(self.result()['ok'])
        self.assertTrue((self.state / 'starts').exists())

    def test_disable_during_start(self):
        self.write_health(stale=True)
        p = self.launch()
        for _ in range(100):
            if (self.state / 'ready').exists():
                break
            time.sleep(.01)
        (self.mod / 'disable').touch()
        self.assertIn('禁用', self.result(p)['error'])
        self.wait_stopped()


if __name__ == '__main__':
    unittest.main()
