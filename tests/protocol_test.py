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
    p.stdin.write('CLEAR unexpected\n')
    p.stdin.flush()
    assert not json.loads(p.stdout.readline())['ok']
    assert len(ask(p, 'STATUS')['points']) == 1  # Rejected commands cannot mutate.
    ask(p, 'CLEAR')
    assert ask(p, 'STATUS')['points'] == []
    imported = os.path.join(state, 'import.dat')
    with open(imported, 'w') as f:
        f.write(exported)
    assert len(ask(p, 'IMPORT ' + json.dumps(imported))['points']) == 1
    ask(p, 'USER 1')
    ask(p, 'USER 0')
    ask(p, 'FRAME 10000 1 1 0 0 0 35 80 0 0.009 0.001 0.4 0')
    assert ask(p, 'EVENT 200000 0 0.009')['storage'] == 'saved'
    p.kill()
    p.wait(timeout=5)
    p = start()
    s = ask(p, 'USER 0')
    assert s['points'][0]['lux'] == 0 and s['points'][0]['brightness'] == 0.009
    ask(p, 'FRAME 20000 1 0 -1 -1 0 35 80 0 0.001 0.001 0.4 0')
    s = ask(p, 'FRAME 50000 1 1 0 0 0 35 80 0 0.001 0.001 0.4 0')
    assert not s['apply'] and s['reason'] == 'wake_settling'
    s = ask(p, 'FRAME 52250 1 1 0 0 0 35 80 0 0.009 0.001 0.4 0')
    assert s['apply'] and abs(s['target'] - 0.009) < 1e-9
    p.stdin.close()
    assert p.wait(timeout=5) == 0
print('Persistence, crash recovery, user isolation, single instance and config rejection passed')

# First-install migration validates before creating the durable v7 configuration.
with tempfile.TemporaryDirectory() as state:
    with open(os.path.join(state, 'legacy-config.conf'), 'w') as f:
        f.write('manual_gamma=2.6\nalpha_fast=0.55\nhyst_low=0.04\n')
    p = subprocess.Popen([exe, state, profile], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    s = ask(p, 'USER 0')
    assert s['config_status'] == 'migrated_legacy', s
    data = ask(p, 'CONFIG_GET')['data']
    assert 'manual_gamma=2.6' in data and 'alpha_fast=0.55' in data
    s = ask(p, 'FRAME2 1000 1 1 100 100 0 35 80 0 0.1 0.001 0.4 0 23')
    assert s['night_scale'] == 0.95
    p.stdin.write('FRAME2 2000 1 1 100 100 0 35 80 0 0.1 0.001 0.4 0 24\n');p.stdin.flush()
    assert not json.loads(p.stdout.readline())['ok']
    p.stdin.close();assert p.wait(timeout=5) == 0
    with open(os.path.join(state, 'legacy-config.conf'), 'w') as f:
        f.write('alpha_fast=nan\n')
    p = subprocess.Popen([exe, state, profile], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    assert 'alpha_fast=0.55' in ask(p, 'CONFIG_GET')['data']  # Existing v7 config wins.
    p.stdin.close();assert p.wait(timeout=5) == 0
print('Legacy tuning migration and local-time frame validation passed')

# The bridge maps an OEM event (e.g. 16.752 fused lux) into the direct front
# coordinate (9.3 lux). The saved value must survive hold expiry and restart.
with tempfile.TemporaryDirectory() as state:
    p = start()
    ask(p, 'USER 0')
    ask(p, 'FRAME2 1000 1 1 9.3 180 0 33 70 0 0.049192 0.001 0.4 0 12')
    ask(p, 'FRAME2 4000 1 1 9.3 180 0 33 70 0 0.049192 0.001 0.4 0 12')
    s = ask(p, 'EVENT 300000 9.3 0.049192')
    assert s['storage'] == 'saved' and s['points'][0]['lux'] == 9.3, s
    s = ask(p, 'FRAME2 40000 1 1 9.3 180 0 33 70 0 0.049192 0.001 0.4 0 12')
    assert s['apply'] and abs(s['target'] - 0.049192) < 1e-8, s
    p.kill();p.wait(timeout=5)
    p = start()
    assert ask(p, 'USER 0')['storage'] == 'loaded'
    ask(p, 'FRAME2 50000 1 1 9.3 180 0 33 70 0 0.049192 0.001 0.4 0 23')
    s = ask(p, 'FRAME2 53000 1 1 9.3 180 0 33 70 0 0.049192 0.001 0.4 0 23')
    assert s['apply'] and abs(s['target'] - 0.049192) < 1e-8, s
    p.stdin.close();assert p.wait(timeout=5) == 0
print('Direct-coordinate preference survives hold expiry and process restart')
