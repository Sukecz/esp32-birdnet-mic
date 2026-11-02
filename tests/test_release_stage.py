from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from release_stage import freeze, verify


class StageTests(unittest.TestCase):
    def test_snapshot_is_independent_idempotent_and_rejects_changes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'source'
            for name in ('web-flasher', 'manual-ota-firmware'):
                (root / name).mkdir(parents=True)
                (root / name / 'firmware.bin').write_bytes(b'verified')
            destination = Path(tmp) / 'release'
            metadata = {'version': '2.05', 'source_commit': 'abc'}
            freeze(root, destination, metadata)
            self.assertEqual(verify(destination)['version'], '2.05')
            self.assertEqual(freeze(root, destination, metadata), destination)
            (root / 'web-flasher/firmware.bin').write_bytes(b'new')
            self.assertEqual((destination / 'web-flasher/firmware.bin').read_bytes(), b'verified')
            with self.assertRaisesRegex(ValueError, 'Refusing'):
                freeze(root, destination, metadata)
            target = destination / 'web-flasher/firmware.bin'
            target.chmod(0o644); target.write_bytes(b'tampered')
            with self.assertRaisesRegex(ValueError, 'changed'):
                verify(destination)
