import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from check_release_texts import check
class ReleaseTextTests(unittest.TestCase):
    def test_installation_copy_has_one_manifest_driven_version(self):
        html='Install BirdNETMic from your browser. <span id="fw-version-chip"></span>'
        check(html,'2.06')
        with self.assertRaises(ValueError): check(html.replace('BirdNETMic from','BirdNETMic 2.04 from'),'2.06')
        with self.assertRaises(ValueError): check(html.replace('fw-version-chip','missing'),'2.06')
