#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the KernelSU archive from this checkout and this run's build artifacts."""
import hashlib
import os
from pathlib import Path
import stat
import subprocess
import zipfile

root = Path(__file__).resolve().parent.parent
out = root / 'dist'
out.mkdir(exist_ok=True)
sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
revision = subprocess.check_output(['git', 'show', '-s', '--format=%ct'], text=True).strip()
artifacts = {
    'bin/ios_brightness_daemon': root / 'build/artifacts/core/ios_brightness_daemon',
    'bridge.jar': root / 'build/artifacts/bridge/bridge.jar',
}
for name, path in artifacts.items():
    if not path.is_file() or not path.stat().st_size:
        raise SystemExit(f'Missing build artifact: {name}')
elf = artifacts['bin/ios_brightness_daemon'].read_bytes()
if elf[:6] != b'\x7fELF\x02\x01' or int.from_bytes(elf[18:20], 'little') != 183:
    raise SystemExit('Core must be a little-endian AArch64 ELF64 executable')
with zipfile.ZipFile(artifacts['bridge.jar']) as jar:
    if 'classes.dex' not in jar.namelist():
        raise SystemExit('Bridge has no DEX bytecode')
files = {str(p.relative_to(root / 'module')): p.read_bytes()
         for p in (root / 'module').rglob('*') if p.is_file()}
files.update({name: p.read_bytes() for name, p in artifacts.items()})
files['LICENSE'] = (root / 'LICENSE').read_bytes()
files['SOURCE.txt'] = (f'GPL-3.0-only\nSource: https://github.com/reF1nd/iOS-Auto-Brightness/tree/{sha}\n'
                       f'Commit: {sha}\nCommit timestamp: {revision}\n').encode()
archive = out / f'iOS-Auto-Brightness-v7.0.0-beta.4-{sha[:8]}.zip'
with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for name, data in sorted(files.items()):
        info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
        mode = 0o755 if name.endswith('.sh') or name == 'bin/ios_brightness_daemon' else 0o644
        info.create_system = 3
        info.external_attr = (stat.S_IFREG | mode) << 16
        info.compress_type = zipfile.ZIP_DEFLATED
        z.writestr(info, data)
with zipfile.ZipFile(archive) as z:
    assert z.testzip() is None
    for name in ['module.prop','customize.sh','service.sh','uninstall.sh','skip_mount',
                 'webroot/index.html','bin/ios_brightness_daemon','bridge.jar','LICENSE','SOURCE.txt']:
        assert name in z.namelist(), name
    assert not any('..' in Path(name).parts or name.startswith('/') for name in z.namelist())
(out / (archive.name + '.sha256')).write_text(hashlib.sha256(archive.read_bytes()).hexdigest() + '  ' + archive.name + '\n')
print(archive)
