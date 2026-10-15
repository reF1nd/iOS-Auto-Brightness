#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise the real installer transaction in isolated Linux fixtures (CI only).

Mock Android service/health and proc identity; use real files, renames, child
processes and flock. Never needs root or modifies the host module directories.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import time
import unittest

SOURCE = Path('module/hot-update.sh').read_text()
BUSYBOX = os.environ.get('HOT_TEST_BUSYBOX')
SHELL = [BUSYBOX, 'sh'] if BUSYBOX else ['bash']


class HotUpdateTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.active = self.root / 'modules/ios_auto_brightness'
        self.stage = self.root / 'modules_update/ios_auto_brightness'
        self.state = self.root / 'state'
        self.proc = self.root / 'proc'
        for path in (self.active.parent, self.stage.parent, self.state, self.proc / 'sys/kernel/random'):
            path.mkdir(parents=True, exist_ok=True)
        (self.proc / 'sys/kernel/random/boot_id').write_text('boot-A\n')
        (self.proc / 'mounts').write_text('')
        self.script = self.root / 'hot-update.sh'
        self.script.write_text(SOURCE.replace('/data/adb/modules/ios_auto_brightness', str(self.active))
                               .replace('/data/adb/modules_update/ios_auto_brightness', str(self.stage))
                               .replace('/data/adb/ios_auto_brightness', str(self.state))
                               .replace('HOT_PROC=/proc', f'HOT_PROC={self.proc}'))
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        self.bb = self.bin / 'busybox'
        self.bb.write_text('#!/bin/sh\nexport IAB_UPDATE_SHELL_READY=$$\n' +
                           (f'exec {shlex.quote(BUSYBOX)} "$@"\n' if BUSYBOX else 'shift\nexec bash "$@"\n'))
        self.bb.chmod(0o755)
        getprop = self.bin / 'getprop'
        getprop.write_text('#!/bin/sh\necho 1\n')
        getprop.chmod(0o755)
        chcon = self.bin / 'chcon'
        chcon.write_text('#!/bin/sh\nexit 0\n')
        chcon.chmod(0o755)
        self.env = dict(os.environ, PATH=f'{self.bin}:{os.environ["PATH"]}', IAB_BB=str(self.bb))
        self.children = []
        self.fake_process(124, 123, 400, ['sh', 'install.sh'])
        self.fake_process(123, 1, 300, ['renamed-manager', 'module', 'install', '/fixture.zip'])

    def tearDown(self):
        (self.state / 'stop').touch()
        for child in self.children:
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.terminate()
                child.wait(timeout=5)
        # Detached service and worker are owned by this temporary fixture only.
        for name in ('mock-service.pid',):
            pidfile = self.state / name
            if pidfile.exists():
                try:
                    os.kill(int(pidfile.read_text()), 15)
                except ProcessLookupError:
                    pass
        self.temp.cleanup()

    def fake_process(self, pid, parent, stamp, argv, status='S'):
        path = self.proc / str(pid)
        path.mkdir(exist_ok=True)
        fields = [status, str(parent)] + ['0'] * 17 + [str(stamp)]
        (path / 'stat').write_text(f'{pid} (test process) ' + ' '.join(fields))
        (path / 'cmdline').write_bytes(b'\0'.join(arg.encode() for arg in argv) + b'\0')

    def shell(self, body, check=True):
        return subprocess.run(SHELL + ['-c', '. "$1" --library; ' + body, 'fixture', str(self.script)],
                              env=self.env, capture_output=True, text=True, check=check, timeout=15)

    def tree(self, path, version):
        (path / 'bin').mkdir(parents=True)
        (path / 'webroot').mkdir()
        (path / 'module.prop').write_text('id=ios_auto_brightness\n')
        (path / 'skip_mount').touch()
        (path / 'customize.sh').write_text('# installer only\n')
        for name in ('bridge.jar', 'webroot/index.html', 'SOURCE.txt', 'bin/ios_brightness_daemon'):
            (path / name).write_text(version)
        (path / 'bin/ios_brightness_daemon').chmod(0o755)
        (path / 'hot-update.sh').write_text(self.script.read_text())
        (path / 'service.sh').write_text(f'''#!/bin/sh
state={shlex.quote(str(self.state))}
exec 9>"$state/supervisor.lock"
flock -n 9 || exit 0
exec 7>"$state/core.lock"
flock -n 7 || exit 1
[ ! -e /proc/$$/fd/8 ] || touch "$state/leaked-update-lock"
rm -f "$state/stop"
echo $$ > "$state/mock-service.pid"
cat "${{0%/*}}/SOURCE.txt" > "$state/running"
trap 'rm -f "$state/running"' EXIT
while [ ! -e "$state/stop" ]; do sleep 0.1; done
''')
        (path / 'control.sh').write_text(f'''#!/bin/sh
state={shlex.quote(str(self.state))}
case "$1" in
stop) touch "$state/stop"; echo '{{"ok":true}}' ;;
health) [ -f "$state/running" ] && echo '{{"health":"healthy"}}' ;;
upgrade-ready)
  [ ! -f "$state/fail-ready" ] && cmp -s "$state/running" "${{0%/*}}/SOURCE.txt" || exit 1
  printf '{{"ready":true,"heartbeat_ms":%s}}\\n' "$(date +%s%3N)" ;;
esac
''')

    def installed(self):
        self.tree(self.active, 'old')
        self.tree(self.stage, 'new')
        (self.state / 'activated-boot').write_text('boot-A')
        for name in ('config.conf', 'learning-0.dat', 'learning-0.dat.bak'):
            (self.state / name).write_text('preserve ' + name)
        child = subprocess.Popen(['bash', str(self.active / 'service.sh')], env=self.env,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.children.append(child)
        self.wait_for(lambda: (self.state / 'running').exists())

    def prepare(self):
        result = subprocess.run(['bash', '-c', 'export IAB_UPDATE_SHELL_READY=$$; exec "$@"',
                                 'fixture'] + SHELL + [str(self.script), 'prepare', str(self.stage), '124', '123', '/fixture.zip', 'true'],
                                env=self.env, capture_output=True, text=True, timeout=70)
        return result

    def wait_for(self, predicate, seconds=10):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if predicate():
                return
            time.sleep(0.1)
        self.fail('Timed out waiting for transaction state')

    def installer_exited(self):
        for pid in (124, 123):
            (self.proc / str(pid) / 'stat').unlink()

    def test_first_install_reinstall_before_reboot(self):
        r = self.shell('hot_classify', check=False)
        self.assertEqual(r.returncode, 2)
        self.assertEqual((self.state / 'first-install-boot').read_text().strip(), 'boot-A')
        # A manager may populate active files and even a stale activation marker.
        self.tree(self.active, 'pending-first')
        (self.state / 'activated-boot').write_text('ancient-boot')
        r = self.shell('hot_health() { return 0; }; hot_classify', check=False)
        self.assertEqual(r.returncode, 2)
        (self.proc / 'sys/kernel/random/boot_id').write_text('boot-B')
        self.assertEqual(self.shell('hot_classify').returncode, 0)

    def test_legacy_running_install_is_update(self):
        self.tree(self.active, 'legacy')
        self.assertEqual(self.shell('hot_health() { return 0; }; hot_classify').returncode, 0)
        self.assertFalse((self.state / 'first-install-boot').exists())

    def test_installer_identity_and_pid_reuse(self):
        r = self.shell('HOT_ZIP=/fixture.zip; hot_find_installer 124; echo "$HOT_PARENT:$HOT_PARENT_STAMP"')
        self.assertEqual(r.stdout.strip(), '123:300')
        self.fake_process(123, 1, 301, ['sh', '-c', 'ksud module install /fixture.zip'])
        self.assertNotEqual(self.shell('HOT_ZIP=/fixture.zip; hot_find_installer 124', check=False).returncode, 0)
        self.fake_process(123, 1, 302, ['ksud', 'module', 'install', '/other.zip'])
        self.assertNotEqual(self.shell('HOT_ZIP=/fixture.zip; hot_find_installer 124', check=False).returncode, 0)
        self.fake_process(123, 1, 303, ['ksud'], status='Z')
        self.assertEqual(self.shell('hot_stamp 123').stdout, '')

    def test_mount_disable_and_unknown_installer_fallback(self):
        self.installed()
        (self.proc / 'mounts').write_text(f'volume {self.active} ext4 rw 0 0\n')
        self.assertEqual(self.prepare().returncode, 2)
        (self.proc / 'mounts').write_text('')
        (self.active / 'disable').touch()
        self.assertEqual(self.prepare().returncode, 2)
        (self.active / 'disable').unlink()
        self.fake_process(123, 1, 300, ['unknown-manager'])
        self.assertEqual(self.prepare().returncode, 2)
        self.assertEqual((self.active / 'SOURCE.txt').read_text(), 'old')
        self.assertFalse((self.state / 'stop').exists())

    def test_success_waits_for_native_installer_and_clears_recreated_marker(self):
        self.installed()
        result = self.prepare()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.active / 'SOURCE.txt').read_text(), 'new')
        self.assertTrue((self.active / 'update').exists())
        self.assertEqual((self.state / 'update-status').read_text().strip(), 'waiting_for_installer')
        # Shell exits first, native installer has yet to recreate update.
        (self.proc / '124/stat').unlink()
        time.sleep(1.2)
        self.assertTrue((self.active / 'update').exists())
        (self.active / 'update').write_text('native marker')
        (self.stage / 'customize.sh').unlink()  # real KSU post-customize cleanup
        self.assertFalse((self.active / 'customize.sh').exists())
        # Reused PID must not be mistaken for the original installer.
        self.fake_process(123, 1, 999, ['unrelated'])
        self.wait_for(lambda: (self.state / 'update-status').read_text().strip() == 'ready')
        self.assertFalse((self.active / 'update').exists())
        self.assertTrue(self.stage.is_dir())
        self.assertFalse((self.state / 'leaked-update-lock').exists())
        for name in ('config.conf', 'learning-0.dat', 'learning-0.dat.bak'):
            self.assertEqual((self.state / name).read_text(), 'preserve ' + name)
        self.shell('exec 8>"$HOT_STATE/update.lock"; flock -n 8')

    def test_changed_stage_never_acknowledged(self):
        self.installed()
        result = self.prepare()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        (self.stage / 'SOURCE.txt').write_text('concurrent-install')
        self.installer_exited()
        self.wait_for(lambda: (self.state / 'update-status').read_text().strip() == 'pending')
        self.assertTrue((self.active / 'update').exists())

    def test_failed_new_service_rolls_back_and_quarantines_stage(self):
        self.installed()
        (self.state / 'fail-ready').touch()
        result = self.prepare()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.active / 'SOURCE.txt').read_text(), 'old')
        self.assertFalse(self.stage.exists())
        self.assertEqual((self.state / 'update-status').read_text().strip(), 'rolled_back')
        self.assertEqual((self.state / 'running').read_text(), 'old')
        self.assertTrue(list((self.state / 'updates').glob('txn.*/failed-stage')))
        self.assertEqual((self.state / 'learning-0.dat').read_text(), 'preserve learning-0.dat')

    def test_service_refuses_first_install_before_reboot(self):
        # Run the actual service through its guard, before any Android bridge call.
        path = self.root / 'service.sh'
        path.write_text(Path('module/service.sh').read_text()
                        .replace('/data/adb/ios_auto_brightness', str(self.state))
                        .replace('/proc/sys/kernel/random/boot_id', str(self.proc / 'sys/kernel/random/boot_id')))
        (self.state / 'first-install-boot').write_text('boot-A')
        result = subprocess.run(['bash', '-c', 'export IAB_SHELL_READY=$$; exec bash "$1"', 'fixture', str(path)],
                                env=self.env, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.state / 'activated-boot').exists())
        self.assertIn('awaiting reboot', (self.state / 'service.log').read_text())


if __name__ == '__main__':
    unittest.main(verbosity=2)
