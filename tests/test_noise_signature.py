"""Run adaptive noise reduction DSP regressions with undefined behavior checks."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class NoiseSignatureTests(unittest.TestCase):
    def test_native_dsp(self):
        with tempfile.TemporaryDirectory() as t:
            binary=Path(t)/'test'
            subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all','-I'+str(ROOT/'esp32-birdnet-mic'),str(ROOT/'tests/noise_signature_test.cpp'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True,timeout=60)
