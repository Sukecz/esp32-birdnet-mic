"""A stale assembled page must fail before browser/host checks and replace results."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class UISourceGateTests(unittest.TestCase):
    def test_empty_filter_is_not_reported_as_success(self):
        result = subprocess.run([sys.executable, str(ROOT / 'tools/test.py'), 'browser',
                                 '--filter', 'no-such-test-should-exist'],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 2)
        self.assertIn('No JavaScript tests match', result.stderr)

    def test_failed_assembly_is_reported_and_stops_suite(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'tools').mkdir()
            for name in ('test.py', 'board_registry.py'):
                shutil.copyfile(ROOT / 'tools' / name, root / 'tools' / name)
            (root / 'tools/assemble_webui.py').write_text('raise SystemExit(3)\n')
            destination = root / 'tmp/test-results'
            destination.mkdir(parents=True)
            (destination / 'host.json').write_text('[{"exit_code":0}]')
            result = subprocess.run([sys.executable, str(root / 'tools/test.py'), 'host'],
                                    cwd=root, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 1)
            report = json.loads((destination / 'host.json').read_text())
            self.assertEqual(len(report), 1)
            self.assertEqual(report[0]['name'], 'UI source assembly')
            self.assertEqual(report[0]['exit_code'], 3)


if __name__ == '__main__':
    unittest.main()
