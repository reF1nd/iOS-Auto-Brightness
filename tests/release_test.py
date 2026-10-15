# SPDX-License-Identifier: GPL-3.0-only
import hashlib
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import release


class ReleaseTest(unittest.TestCase):
    def test_version_and_notes(self):
        module = 'id=ios_auto_brightness\nversion=v7.0.0-beta.13\nversionCode=70013\n'
        self.assertTrue(release.validate_version('v7.0.0-beta.13', module))
        self.assertFalse(release.validate_version('v7.0.0', module.replace('-beta.13', '')))
        for tag in ['main', 'v7.0.0', 'v7.0.0-beta.13\n', 'v7.0.0;echo bad']:
            with self.assertRaises(ValueError):
                release.validate_version(tag, module)
        with self.assertRaises(ValueError):
            release.validate_version('v7.0.0-beta.13', module + 'version=v7.0.0-beta.13\n')
        log = '# History\n\n## v7.0.0-beta.13\n\nNew features.\n\n## v7.0.0-beta.12\n\nOlder.\n'
        self.assertEqual(release.notes('v7.0.0-beta.13', log), 'New features.')
        for invalid in ['', '## v7.0.0-beta.13\n\n', log + '\n## v7.0.0-beta.13\nDuplicate.']:
            with self.assertRaises(ValueError):
                release.notes('v7.0.0-beta.13', invalid)

    def test_package_provenance_and_checksum(self):
        tag, sha = 'v7.0.0-beta.13', 'a' * 40
        module = f'id=ios_auto_brightness\nversion={tag}\nversionCode=70013\n'
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            archive = directory / f'iOS-Auto-Brightness-{tag}-{sha[:8]}.zip'
            checksum = directory / (archive.name + '.sha256')

            def write(source=sha, metadata=module):
                with zipfile.ZipFile(archive, 'w') as package:
                    package.writestr('module.prop', metadata)
                    package.writestr('SOURCE.txt', f'Commit: {source}\n')
                checksum.write_text(hashlib.sha256(archive.read_bytes()).hexdigest() + '  ' + archive.name + '\n')

            write()
            self.assertEqual(release.verify(tag, sha, directory, module), (archive, checksum))
            write(source='b' * 40)
            with self.assertRaises(ValueError):
                release.verify(tag, sha, directory, module)
            write(metadata=module.replace('70013', '1'))
            with self.assertRaises(ValueError):
                release.verify(tag, sha, directory, module)
            write()
            checksum.write_text('0' * 64 + '  ' + archive.name)
            with self.assertRaises(ValueError):
                release.verify(tag, sha, directory, module)
            write()
            (directory / 'unexpected.zip').write_bytes(b'other build')
            with self.assertRaises(ValueError):
                release.verify(tag, sha, directory, module)


if __name__ == '__main__':
    unittest.main()
