from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class ValidationTests(unittest.TestCase):
    def test_production_validation_module(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = str(Path(tmp) / 'validation')
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            str(ROOT / 'tests/settings_validation_test.cpp'), '-o', binary], check=True)
            subprocess.run([binary], check=True)
