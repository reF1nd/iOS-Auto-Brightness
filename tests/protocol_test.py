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
    probe = json.loads(subprocess.check_output([exe, '--panel-read'], text=True))
    assert not probe['ok'] and 'readback' in probe['error'], probe  # no vendor node on CI host
    p = start()
    p.stdin.write('PANEL_READ\n');p.stdin.flush()
    probe = json.loads(p.stdout.readline())
    assert not probe['ok'], probe
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
    assert s['core_pid'] == p.pid and s['cpu_core_percent'] >= 0
    assert ask(p, 'SOURCE rear')['points'] == []
    ask(p, 'FRAME 2000 1 1 100 100 0 35 80 0 0.1 0.001 0.4 0')
    assert ask(p, 'EVENT 123457 100 0.2')['storage'] == 'saved'
    assert ask(p, 'SOURCE default')['points'][0]['brightness'] == 0.15
    assert ask(p, 'SOURCE rear')['points'][0]['brightness'] == 0.2
    p.kill();p.wait(timeout=5);p=start();ask(p, 'USER 0')
    assert ask(p, 'SOURCE rear')['points'][0]['brightness'] == 0.2
    ask(p, 'SOURCE default')
    p.stdin.write('SOURCE ../bad\n');p.stdin.flush()
    assert not json.loads(p.stdout.readline())['ok']
    # Thermal-blocked frames must never accept learning or produce an override.
    protected=ask(p, 'FRAME 3000 1 1 100 100 0 35 80 0 0.05 0.001 0.08 1')
    assert not protected['apply']
    assert ask(p, 'EVENT 123458 100 0.05').get('accepted') is False
    assert ask(p, 'STATUS')['points'][0]['brightness'] == 0.15

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

# A shaded front sensor must neither lower output nor write a learning point.
with tempfile.TemporaryDirectory() as state:
    p = start();ask(p, 'USER 0')
    ask(p, 'FRAME2 1000 1 1 9.3 114 0 34 67 0 0.08925 0.001 0.4 0 12')
    ask(p, 'EVENT 400000 9.3 0.08925')
    ask(p, 'FRAME2 40000 1 1 9.3 114 0 34 67 0 0.08925 0.001 0.4 0 12')
    for now in range(40250,100000,250):
        s = ask(p, f'FRAME2 {now} 1 1 1.825 89 0 34 67 0 0.08925 0.001 0.4 0 12')
        assert s['ambient_guard'] and abs(s['target'] - 0.08925) < 1e-8, s
    assert ask(p, 'EVENT 410000 1.825 0.04')['accepted'] is False
    assert len(ask(p, 'STATUS')['points']) == 1
    p.stdin.close();assert p.wait(timeout=5) == 0
print('Ambient guard blocks dimming and learning across the process protocol')

# Recovery commands must work with no bridge, malformed config and absent profile.
with tempfile.TemporaryDirectory() as state:
    config = os.path.join(state, 'config.conf')
    with open(config, 'w') as out:
        out.write('broken input')
    result = subprocess.run([exe, '--config', 'save', config], input='alpha_fast=0.6\n', text=True, capture_output=True)
    assert result.returncode == 0, result.stderr
    before = open(config).read()
    assert 'alpha_fast=0.6' in before
    assert os.stat(config).st_mode & 0o777 == 0o600
    result = subprocess.run([exe, '--config', 'save', config], input='alpha_fast=9\n', text=True, capture_output=True)
    assert result.returncode != 0 and open(config).read() == before
    result = subprocess.run([exe, '--config', 'check', config], text=True, capture_output=True)
    assert result.returncode == 0 and result.stdout == before

# FRAME3 keeps fused learning separate and preserves it across process restart.
with tempfile.TemporaryDirectory() as state:
    p = start()
    ask(p, 'USER 0')
    ask(p, 'SOURCE dualmaxv1')
    ask(p, 'FRAME3 1000 1 1 9.774 9.774 0 35 80 0 0.03 0.001 0.4 0 12 0.738')
    assert ask(p, 'EVENT 500000 9.774 0.03')['storage'] == 'saved'
    assert ask(p, 'SOURCE default')['points'] == []
    p.kill(); p.wait(timeout=5); p = start(); ask(p, 'USER 0')
    saved = ask(p, 'SOURCE dualmaxv1')['points'][0]
    assert saved['lux'] == 9.774 and saved['brightness'] == 0.03
    ask(p, 'FRAME3 1000 1 1 9.774 9.774 0 35 80 0 0.03 0.001 0.4 0 12 0')
    for now in range(1250, 11000, 250):
        s = ask(p, f'FRAME3 {now} 1 1 9.774 9.774 0 35 80 0 0.03 0.001 0.4 0 12 0')
    assert s['apply'] and abs(s['target'] - 0.03) < 1e-7 and not s['ambient_guard'], s
    assert ask(p, 'FRAME3 11000 1 1 9.774 9.774 1 35 80 0 0.03 0.001 0.4 0 12 0')['reason'] == 'proximity_pending'
    assert ask(p, 'FRAME3 11400 1 1 9.774 9.774 0 35 80 0 0.03 0.001 0.4 0 12 0')['apply']
    assert not ask(p, 'FRAME3 11500 1 1 9.774 9.774 1 35 80 0 0.03 0.001 0.4 1 12 0')['apply']
    p.stdin.close(); assert p.wait(timeout=5) == 0
print('Fused coordinate persistence, wake, proximity debounce and protection passed')

# Reference model uses versioned observations and restores the same fitted curve
# after a real process replacement, while leaving legacy data isolated.
with tempfile.TemporaryDirectory() as state:
    def ref_start():
        return subprocess.Popen([exe,state,profile],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    p=ref_start();ask(p,'POLICY v68');ask(p,'USER 0')
    ask(p,'FRAME 1000 1 1 10 10 0 30 80 0 0.8 0.001 1 0')
    ask(p,'SLIDER 80 10000');s=ask(p,'SLIDER 110 11000')
    assert s['storage']=='saved_reference_curve' and len(s['points'])==1,s
    first=s['curve'];p.kill();p.wait(timeout=5)
    p=ref_start();ask(p,'POLICY v68');s=ask(p,'USER 0')
    assert s['storage']=='loaded' and s['curve']==first,s
    assert 'IAB_CURVE_OBSERVATIONS_V2' in ask(p,'EXPORT')['data']
    p.kill();p.wait(timeout=5)
