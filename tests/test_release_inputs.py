from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import build_receipt
import verify_release_inputs
from board_registry import boards


class ReleaseInputTests(unittest.TestCase):
    def test_packaged_payload_must_match_source_bound_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            board = next(b for b in boards() if b['id'] == 'c6')
            directory = root / 'esp32-birdnet-mic/build' / board['fqbn'].replace(':', '.')
            directory.mkdir(parents=True)
            web = root / 'web-flasher'
            web.mkdir()
            (directory / 'build.options.json').write_text('{}')
            pairs = [('firmware-app-c6.bin', 'esp32-birdnet-mic.ino.bin'),
                     ('firmware-c6.bin', 'esp32-birdnet-mic.ino.merged.bin'),
                     ('bootloader-c6.bin', 'esp32-birdnet-mic.ino.bootloader.bin'),
                     ('partitions-c6.bin', 'esp32-birdnet-mic.ino.partitions.bin'),
                     ('boot_app0-c6.bin', 'boot_app0.bin')]
            for public, local in pairs:
                (directory / local).write_bytes(local.encode())
                (web / public).write_bytes(local.encode())
            with patch.object(build_receipt, 'source_digest', return_value='source'), \
                 patch.object(verify_release_inputs, 'ROOT', root), \
                 patch.object(verify_release_inputs, 'boards', return_value=[board]):
                build_receipt.record(directory)
                verify_release_inputs.main()
                (web / 'firmware-app-c6.bin').write_bytes(b'other firmware')
                with self.assertRaisesRegex(ValueError, 'differs'):
                    verify_release_inputs.main()


if __name__ == '__main__':
    unittest.main()
