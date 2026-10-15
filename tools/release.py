#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate release identity and artifacts without executing packaged code."""
import hashlib
import re
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TAG = re.compile(r'v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*))?')


def properties(text):
    result = {}
    for line in text.splitlines():
        if not line.strip() or line.startswith('#'):
            continue
        key, value = line.split('=', 1)
        if key in result:
            raise ValueError('Duplicate module property')
        result[key] = value
    return result


def validate_version(tag, module):
    match = TAG.fullmatch(tag)
    if not match or properties(module).get('version') != tag:
        raise ValueError('Tag must be a version matching module.prop exactly')
    return bool(match.group(4))


def notes(tag, changelog):
    if not TAG.fullmatch(tag):
        raise ValueError('Invalid release tag')
    sections = re.findall(r'^## ([^\n]+)\n(.*?)(?=^## |\Z)', changelog, re.M | re.S)
    matches = [body.strip() for title, body in sections if title == tag]
    if len(matches) != 1 or not matches[0]:
        raise ValueError('Exactly one nonempty changelog section required')
    return matches[0]


def verify(tag, sha, directory, module):
    validate_version(tag, module)
    if not re.fullmatch(r'[0-9a-f]{40}', sha):
        raise ValueError('Invalid source commit')
    name = f'iOS-Auto-Brightness-{tag}-{sha[:8]}.zip'
    archive = directory / name
    checksum = directory / (name + '.sha256')
    if sorted(p.name for p in directory.iterdir()) != sorted([name, checksum.name]):
        raise ValueError('Unexpected or missing release artifacts')
    expected = hashlib.sha256(archive.read_bytes()).hexdigest() + '  ' + name
    if checksum.read_text().strip() != expected:
        raise ValueError('Archive checksum mismatch')
    with zipfile.ZipFile(archive) as package:
        if package.testzip() is not None:
            raise ValueError('Corrupt archive')
        if len(package.namelist()) != len(set(package.namelist())):
            raise ValueError('Duplicate archive entries')
        if package.read('module.prop').decode() != module:
            raise ValueError('Packaged module metadata differs from tag')
        commits = re.findall(r'^Commit: (.+)$', package.read('SOURCE.txt').decode(), re.M)
        if commits != [sha]:
            raise ValueError('Packaged source differs from tested commit')
    return archive, checksum


if __name__ == '__main__':
    command, tag = sys.argv[1:3]
    module = (ROOT / 'module/module.prop').read_text()
    prerelease = validate_version(tag, module)
    body = notes(tag, (ROOT / 'CHANGELOG.md').read_text())
    if command == 'metadata':
        print(f'tag={tag}\nprerelease={str(prerelease).lower()}')
    elif command == 'notes':
        # Release bodies need repository links, not relative document links.
        print(body.replace('](docs/', f'](https://github.com/reF1nd/iOS-Auto-Brightness/blob/{tag}/docs/'))
    elif command == 'verify':
        verify(tag, sys.argv[3], Path(sys.argv[4]), module)
    else:
        raise SystemExit('Unknown release command')
