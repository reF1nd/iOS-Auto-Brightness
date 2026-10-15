# SPDX-License-Identifier: GPL-3.0-only
"""Exercise the real daemon across process replacement, not only storage helpers."""
import json
import os
import subprocess
import sys
import tempfile

exe, profile = sys.argv[1:]
with tempfile.TemporaryDirectory() as state:
    def start():
        return subprocess.Popen([exe, state, profile], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    def ask(p, line):
        p.stdin.write(line + '\n')
        p.stdin.flush()
        response = json.loads(p.stdout.readline())
        assert response['ok'], response
        return response
    p = start()
    ask(p, 'USER 0')
    ask(p, 'FRAME 1000 1 1 100 100 0 35 80 0 0.1 0.001 0.4 0')
    s = ask(p, 'EVENT 123456 100 0.15')
    assert s['storage'] == 'saved' and len(s['points']) == 1, s
    p.kill()  # No shutdown hook is allowed to make persistence work.
    p.wait(timeout=5)
    p = start()
    s = ask(p, 'USER 0')
    assert s['points'][0]['brightness'] == 0.15 and s['storage'] == 'loaded', s
    exported = ask(p, 'EXPORT')['data']
    s = ask(p, 'USER 1')
    assert s['points'] == [], s
    s = ask(p, 'USER 0')
    assert len(s['points']) == 1
    # Second instance must not steal the state lock.
    duplicate = subprocess.run([exe, state, profile], input='', text=True, capture_output=True, timeout=5)
    assert duplicate.returncode != 0
    bad = os.path.join(state, 'bad.conf')
    with open(bad, 'w') as f:
        f.write('alpha_fast=nan\n')
    p.stdin.write('CONFIG ' + json.dumps(bad) + '\n')
    p.stdin.flush()
    assert not json.loads(p.stdout.readline())['ok']
    assert 'alpha_fast=0.45' in ask(p, 'CONFIG_GET')['data']
    ask(p, 'CLEAR')
    assert ask(p, 'STATUS')['points'] == []
    imported = os.path.join(state, 'import.dat')
    with open(imported, 'w') as f:
        f.write(exported)
    assert len(ask(p, 'IMPORT ' + json.dumps(imported))['points']) == 1
    p.stdin.close()
    assert p.wait(timeout=5) == 0
print('Persistence, crash recovery, user isolation, single instance and config rejection passed')
