from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import build_receipt


class BuildReceiptTests(unittest.TestCase):
    def test_stale_sources_and_modified_artifacts_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            binary = directory / 'app.bin'
            binary.write_bytes(b'firmware')
            (directory / 'build.options.json').write_text('{}')
            with patch.object(build_receipt, 'source_digest', return_value='source-a'):
                build_receipt.record(directory)
                build_receipt.verify(directory)
                binary.write_bytes(b'tampered')
                with self.assertRaisesRegex(ValueError, 'artifacts changed'):
                    build_receipt.verify(directory)
                binary.write_bytes(b'firmware')
            with patch.object(build_receipt, 'source_digest', return_value='source-b'):
                with self.assertRaisesRegex(ValueError, 'Stale build'):
                    build_receipt.verify(directory)
                with self.assertRaisesRegex(ValueError, 'during compilation'):
                    build_receipt.record(directory, expected_source='source-a')


if __name__ == '__main__':
    unittest.main()
