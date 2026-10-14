"""Compile and exercise the production TCP writer on real and scripted sockets."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class CooperativeTcpTests(unittest.TestCase):
    def test_partial_frames_backpressure_and_recovery(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = pathlib.Path(temp) / 'tcp-writer'
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'esp32-birdnet-mic'),
                            str(ROOT / 'tests/cooperative_tcp_writer_test.cpp'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
