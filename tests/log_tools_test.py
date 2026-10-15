# SPDX-License-Identifier: GPL-3.0-only
"""Run the actual offline log commands with an isolated state directory in CI."""
import os
from pathlib import Path
import subprocess
import tempfile

with tempfile.TemporaryDirectory() as temp:
    state = Path(temp)
    script = state / 'control.sh'
    script.write_text(Path('module/control.sh').read_text().replace('/data/adb/ios_auto_brightness', temp))
    def run(command):
        # Preserve PID through exec to use host bash rather than an Android root-manager shell.
        return subprocess.run(['bash', '-c', 'export IAB_CONTROL_SHELL_READY=$$; exec bash "$1" "$2"', 'test', str(script), command],
                              text=True, capture_output=True, check=True, timeout=5).stdout
    (state / 'service.log.2').write_text('oldest\n')
    (state / 'service.log.1').write_text(''.join(f'backup-{i}\n' for i in range(90)))
    (state / 'service.log').write_text('newest\n')
    recent = run('log-read').splitlines()
    assert len(recent) == 80 and recent[0] == 'backup-11' and recent[-1] == 'newest', recent
    learning = state / 'learning-test.dat'
    learning.write_text('keep learning')
    assert '"ok":true' in run('log-clear')
    assert (state / 'service.log').read_text() == ''
    assert not (state / 'service.log.1').exists() and not (state / 'service.log.2').exists()
    assert learning.read_text() == 'keep learning'
    assert run('log-read') == ''
print('Offline log read across rotation and complete clear passed')
