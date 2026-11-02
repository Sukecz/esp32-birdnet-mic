from datetime import datetime
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from board_registry import target
from qualify_runtime import sample, summarize


class RuntimeQualificationTests(unittest.TestCase):
    def fixture(self):
        status = {'board_id': 'xiao-esp32c6', 'fw_version': '2.05', 'time_sync_enabled': False,
                  'streaming': True, 'internal_heap_free_kb': 100, 'internal_heap_min_kb': 90,
                  'internal_heap_largest_kb': 80, 'psram_total_kb': 0, 'psram_free_kb': 0}
        audio = {'producer_running': True, 'mic_signal': 'ok', 'i2s_error_count': 0, 'rb_drops': 3}
        return status, audio

    def test_quantized_memory_and_observed_minima(self):
        status, audio = self.fixture()
        a = sample(status, audio, target('c6'))
        self.assertEqual(a['internal_heap_free_bytes'], 102400)
        self.assertTrue(summarize([a])['ok'])
        status['internal_heap_free_kb'] = 95
        b = sample(status, audio, target('c6'))
        self.assertEqual(summarize([a, b])['minima']['internal_heap_free_bytes'], 95*1024)

    def test_capacity_errors_drop_growth_and_missing_samples_fail(self):
        status, audio = self.fixture()
        a = sample(status, audio, target('c6'))
        status['psram_total_kb'] = 8192
        self.assertFalse(summarize([sample(status, audio, target('c6'))])['ok'])
        status['psram_total_kb'] = 0; audio['rb_drops'] += 1
        self.assertFalse(summarize([a, sample(status, audio, target('c6'))])['ok'])
        self.assertFalse(summarize([])['ok'])
