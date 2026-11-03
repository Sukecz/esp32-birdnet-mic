from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from board_registry import boards, matrix, target
from check_memory_budget import partitions, validate


class BoardBudgetTests(unittest.TestCase):
    def fixture(self, board='c6', no_psram=False):
        b = target(board, no_psram)
        slot = b['ota_slot_bytes']
        return b, [(0, 0x10, 0x10000, slot, 'app0'),
                   (0, 0x11, 0x10000 + slot, slot, 'app1')], {'.dram0.data': 20000, '.dram0.bss': 60000, '.iram0.text': 100000}

    def test_s31_uses_real_flash_and_fixed_psram_configuration(self):
        b = target('s31')
        self.assertEqual(b['flash_bytes'], 16 * 1024 * 1024)
        self.assertEqual(b['enabled_psram_bytes'], 16 * 1024 * 1024)
        self.assertIn('FlashSize=16M', b['fqbn'])
        self.assertNotIn('PSRAM=', b['fqbn'])
        with self.assertRaises(ValueError):
            target('s31', True)

    def test_matrix_includes_psram_fallbacks(self):
        targets = matrix()['include']
        self.assertEqual(len(targets), 7)
        self.assertEqual(target('s3')['fqbn'].split('PSRAM=')[1], 'opi')
        self.assertEqual(target('c5', True)['enabled_psram_bytes'], 0)

    def test_all_targets_fit_and_app_boundary_is_exact(self):
        for board in boards():
            b, table, sections = self.fixture(board['id'])
            self.assertTrue(validate(b, b['ota_slot_bytes'], table, sections)['ok'])
            self.assertFalse(validate(b, b['ota_slot_bytes'] + 1, table, sections)['ok'])

    def test_wrong_partition_and_overlap_rejected(self):
        b, table, sections = self.fixture()
        for bad in ([(0, 0x10, 0x10000, 1310720, 'app0')],
                    [table[0], (0, 0x11, 0x10000, table[1][3], 'app1')],
                    table + [(1, 2, b['flash_bytes'], 4096, 'outside')]):
            self.assertFalse(validate(b, 100, bad, sections)['ok'])

    def test_static_ram_and_disabled_psram_rejected(self):
        b, table, sections = self.fixture('s3', True)
        for extra in ({'.dram0.bss': b['static_data_limit_bytes']},
                      {'.iram0.text': b['sram_bytes']}, {'.ext_ram.bss': 1}):
            self.assertFalse(validate(b, 100, table, {**sections, **extra})['ok'])
        self.assertFalse(validate(b, 100, table, {})['ok'])

    def test_binary_partition_parser(self):
        entry = struct.pack('<HBBII16sI', 0x50aa, 0, 0x10, 65536, 2031616, b'app0', 0)
        self.assertEqual(partitions(entry + b'\xff'*32)[0], (0, 0x10, 65536, 2031616, 'app0'))
        with self.assertRaises(ValueError):
            partitions(b'bad')

    def test_linker_address_placeholder_is_not_psram_storage(self):
        b, table, sections = self.fixture('s3', True)
        sections['.ext_ram.dummy'] = 1245152
        self.assertTrue(validate(b, 100, table, sections)['ok'])


if __name__ == '__main__':
    unittest.main()
