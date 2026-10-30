#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compile zygisk/java into build/artifacts/zygisk/anchors.dex with javac and d8."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
sdk = Path(os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT') or '')
build_tools = os.environ.get('IAB_BUILD_TOOLS', '35.0.0')
platform = os.environ.get('IAB_ANDROID_PLATFORM', '35')
android_jar = sdk / f'platforms/android-{platform}/android.jar'
d8 = sdk / f'build-tools/{build_tools}/d8'
for tool in (android_jar, d8):
    if not tool.is_file():
        raise SystemExit(f'Missing {tool}; set ANDROID_HOME and install build-tools;{build_tools} platforms;android-{platform}')
sources = sorted(str(p) for p in (root / 'zygisk/java').rglob('*.java'))
out = root / 'build/artifacts/zygisk'
out.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory() as temporary:
    classes = Path(temporary) / 'classes'
    dex = Path(temporary) / 'dex'
    dex.mkdir()
    subprocess.run(['javac', '--release', '11', '-Xlint:all', '-Werror', '-implicit:none', '-cp', str(android_jar),
                    '-d', str(classes)] + sources, check=True)
    subprocess.run([str(d8), '--release', '--min-api', '31', '--lib', str(android_jar), '--output', str(dex)] +
                   sorted(str(p) for p in classes.rglob('*.class')), check=True)
    shutil.copyfile(dex / 'classes.dex', out / 'anchors.dex')
print(out / 'anchors.dex')
