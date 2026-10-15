#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""CI-only real-shell cgroup planner and verified-launch barrier fixtures."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

BUSYBOX = os.environ.get('HOT_TEST_BUSYBOX')
SHELL = [BUSYBOX, 'sh'] if BUSYBOX else ['bash']


class ProcessGroupTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.groups = self.root / 'cgroup'
        self.mounts = self.root / 'mountinfo'
        self.groups.write_text('0::/apps/uid_10556/pid_10141\n4:memory:/mimd/uid_10556\n3:cpu:/background\n')
        self.v2 = self.root / 'unified'
        self.v1 = self.root / 'memory'
        for directory in (self.v1, self.v2):
            directory.mkdir()
            (directory / 'cgroup.procs').touch()
        self.mounts.write_text(
            f'65 44 0:28 / {self.v2} rw shared:21 - cgroup2 none rw\n'
            f'71 41 0:31 / {self.v1} rw shared:24 - cgroup none rw,memory\n')
        source = Path('module/process-group.sh').read_text()
        source = source.replace('/proc/$$/cgroup', str(self.groups)).replace('/proc/$$/mountinfo', str(self.mounts))
        self.script = self.root / 'process-group.sh'
        self.script.write_text(source)

    def tearDown(self):
        self.temp.cleanup()

    def run_shell(self, body):
        return subprocess.run(SHELL + ['-c', '. "$1"; ' + body, 'fixture', str(self.script)],
                              capture_output=True, text=True, timeout=5)

    def targets(self):
        return self.run_shell('iab_cgroup_targets "$(iab_cgroup_membership)"')

    def test_hybrid_oem_groups_only(self):
        result = self.targets()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(set(result.stdout.splitlines()), {str(self.v1 / 'cgroup.procs'), str(self.v2 / 'cgroup.procs')})

    def test_boot_or_adb_root_needs_no_move(self):
        self.groups.write_text('0::/\n4:memory:/apps\n3:cpu:/background\n')
        result = self.run_shell('iab_detach_cgroups && echo "$IAB_CGROUP_MOVED"')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), '0')

    def test_co_mounted_v1_controllers(self):
        self.groups.write_text('2:cpu,cpuacct:/uid_10001/pid_23\n')
        self.mounts.write_text(f'5 1 0:2 / {self.v1} rw - cgroup none rw,cpuacct,cpu\n')
        self.assertEqual(self.targets().stdout.strip(), str(self.v1 / 'cgroup.procs'))

    def test_namespace_without_hierarchy_root_fails(self):
        self.mounts.write_text(self.mounts.read_text().replace(' / ', ' /apps/uid_10556 '))
        self.assertNotEqual(self.targets().returncode, 0)

    def test_missing_controller_mount_fails_before_any_move(self):
        self.mounts.write_text(self.mounts.read_text().splitlines()[0] + '\n')
        self.assertNotEqual(self.run_shell('iab_detach_cgroups').returncode, 0)
        self.assertEqual((self.v2 / 'cgroup.procs').read_text(), '')

    def test_bad_or_unreadable_membership_fails(self):
        for text in ('', 'malformed\n', '0::relative\n'):
            self.groups.write_text(text)
            self.assertNotEqual(self.run_shell('iab_detach_cgroups').returncode, 0)
        self.groups.unlink()
        self.assertNotEqual(self.run_shell('iab_detach_cgroups').returncode, 0)

    def test_success_requires_kernel_membership_change(self):
        # A successful write alone is insufficient (ordinary fixture files do
        # not change /proc membership). No dependent command may run.
        result = self.run_shell('iab_detach_cgroups && echo launched')
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('launched', result.stdout)

    def test_successful_detach_and_inheritance_barrier(self):
        group = shlex.quote(str(self.groups))
        result = self.run_shell('iab_cgroup_write() { printf "%s\\n" "$$" > "$1"; '
                                f'printf "0::/\\n4:memory:/\\n3:cpu:/background\\n" > {group}; }}; '
                                'iab_detach_cgroups && echo "launch:$IAB_CGROUP_MOVED"')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), 'launch:1')
        for directory in (self.v1, self.v2):
            self.assertTrue((directory / 'cgroup.procs').read_text().strip().isdigit())

    def test_write_failure_blocks_launch(self):
        result = self.run_shell('iab_cgroup_write() { return 1; }; iab_detach_cgroups && echo launched')
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('launched', result.stdout)


if __name__ == '__main__':
    unittest.main()
