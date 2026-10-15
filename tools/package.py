#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the KernelSU archive from this checkout and this run's build artifacts."""
import hashlib
import os
import re
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
    'bridge.jar': root / 'build/artifacts/bridge/bridge.jar',
}
for abi, elf_class, machine in [('arm64-v8a',2,183),('armeabi-v7a',1,40),('x86_64',2,62),('x86',1,3)]:
    path=root / f'build/artifacts/core/ios-auto-brightness-android-{abi}/ios_brightness_daemon'
    elf=path.read_bytes()
    if elf[:6] != b'\x7fELF'+bytes([elf_class,1]) or int.from_bytes(elf[18:20], 'little') != machine:
        raise SystemExit(f'Wrong ELF architecture for {abi}')
    artifacts[f'bin/{abi}/ios_brightness_daemon']=path
for name,path in artifacts.items():
    if not path.is_file() or not path.stat().st_size:
        raise SystemExit(f'Missing build artifact: {name}')
with zipfile.ZipFile(artifacts['bridge.jar']) as jar:
    if 'classes.dex' not in jar.namelist():
        raise SystemExit('Bridge has no DEX bytecode')
files = {str(p.relative_to(root / 'module')): p.read_bytes()
         for p in (root / 'module').rglob('*') if p.is_file()}
files.update({name: p.read_bytes() for name, p in artifacts.items()})
files['LICENSE'] = (root / 'LICENSE').read_bytes()
files['SOURCE.txt'] = (f'GPL-3.0-only\nSource: https://github.com/reF1nd/iOS-Auto-Brightness/tree/{sha}\n'
                       f'Commit: {sha}\nCommit timestamp: {revision}\n').encode()
versions=re.findall(r'^version=(.+)$',files['module.prop'].decode(),re.MULTILINE)
if len(versions)!=1 or not re.fullmatch(r'v[0-9A-Za-z.+-]+',versions[0]):
    raise SystemExit('Invalid or ambiguous module version')
archive = out / f'iOS-Auto-Brightness-{versions[0]}-{sha[:8]}.zip'
with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for name, data in sorted(files.items()):
        info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
        mode = 0o755 if name.endswith('.sh') or name.endswith('/ios_brightness_daemon') else 0o644
        info.create_system = 3
        info.external_attr = (stat.S_IFREG | mode) << 16
        info.compress_type = zipfile.ZIP_DEFLATED
        z.writestr(info, data)
with zipfile.ZipFile(archive) as z:
    assert z.testzip() is None
    for name in ['module.prop','customize.sh','service.sh','uninstall.sh','skip_mount',
                 'webroot/index.html','bin/arm64-v8a/ios_brightness_daemon','bridge.jar','LICENSE','SOURCE.txt']:
        assert name in z.namelist(), name
    assert not any('..' in Path(name).parts or name.startswith('/') for name in z.namelist())
(out / (archive.name + '.sha256')).write_text(hashlib.sha256(archive.read_bytes()).hexdigest() + '  ' + archive.name + '\n')
print(archive)
