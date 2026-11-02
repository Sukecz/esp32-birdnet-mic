"""A bad later target must not replace any earlier board's release payload."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PackagingPreflightTests(unittest.TestCase):
    def test_later_memory_failure_preserves_existing_payloads(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for directory in ('tools', 'web-flasher', 'esp32-birdnet-mic/tools', 'bin'):
                (root / directory).mkdir(parents=True)
            for name in ('package_firmware.sh', 'board_registry.py'):
                shutil.copyfile(ROOT / 'tools' / name, root / 'tools' / name)
            shutil.copyfile(ROOT / 'boards.json', root / 'boards.json')
            for name in ('check_build_dependencies.py', 'build_receipt.py'):
                (root / 'tools' / name).write_text('# Test fixture: dependency/receipt checks covered separately.\n')
            (root / 'tools/check_memory_budget.py').write_text(
                "import sys\nraise SystemExit(1 if sys.argv[1] == 's3' else 0)\n")
            for path in (root / 'bin/arduino-cli', root / 'esp32-birdnet-mic/tools/gen_webui_gzip_header.sh'):
                path.write_text('#!/bin/sh\nexit 0\n')
                path.chmod(0o755)
            (root / 'esp32-birdnet-mic/esp32-birdnet-mic.ino').write_text('#define FW_VERSION "2.04"\n')
            payload = root / 'web-flasher/bootloader-c3.bin'
            payload.write_bytes(b'previous verified release')
            for board in json.loads((root / 'boards.json').read_text())['boards']:
                build = root / 'esp32-birdnet-mic/build' / board['fqbn'].replace(':', '.')
                build.mkdir(parents=True)
                for name in ('esp32-birdnet-mic.ino.bin', 'esp32-birdnet-mic.ino.merged.bin',
                             'esp32-birdnet-mic.ino.bootloader.bin', 'esp32-birdnet-mic.ino.partitions.bin',
                             'boot_app0.bin', 'flash_args'):
                    (build / name).write_bytes(b'candidate')
            result = subprocess.run(['bash', str(root / 'tools/package_firmware.sh'), '--skip-build'],
                                    env={**os.environ, 'PATH': str(root / 'bin') + os.pathsep + os.environ['PATH']},
                                    capture_output=True, text=True, timeout=15)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Packaging existing ESP32-S3 build', result.stdout)
            self.assertEqual(payload.read_bytes(), b'previous verified release')


if __name__ == '__main__':
    unittest.main()
