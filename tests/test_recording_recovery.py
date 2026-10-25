from pathlib import Path
import importlib.util
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('recover_recording', ROOT/'tools/recover_recording.py')
recovery = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recovery)


class RecoveryTests(unittest.TestCase):
    def test_stale_checkpoint_odd_tail_and_original_preserved(self):
        header = bytearray(44)
        header[:4] = b'RIFF'
        header[8:16] = b'WAVEfmt '
        header[36:40] = b'data'
        struct.pack_into('<IHHIIHH', header, 16, 16, 1, 1, 48000, 96000, 2, 16)
        with tempfile.TemporaryDirectory() as temp:
            source, dest = Path(temp)/'test.wav.part', Path(temp)/'recovered.wav'
            original = bytes(header) + b'\x00\x80\xff\x7f\x00'
            source.write_bytes(original)
            self.assertEqual(recovery.recover(source, dest), 2)
            self.assertEqual(source.read_bytes(), original)
            self.assertEqual(struct.unpack_from('<I', dest.read_bytes(), 40)[0], 4)
            self.assertEqual(dest.read_bytes()[44:], original[44:48])
            with self.assertRaises(FileExistsError):
                recovery.recover(source, dest)
            with self.assertRaises(FileExistsError):
                recovery.recover(source, source)
            source.write_bytes(b'bad')
            with self.assertRaises(ValueError):
                recovery.recover(source, Path(temp)/'bad.wav')


if __name__ == '__main__':
    unittest.main()
