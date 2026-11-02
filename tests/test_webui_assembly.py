"""Source modules must reproduce the served UI and fail safely on invalid includes."""
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from assemble_webui import assemble, OUTPUT


class WebUIAssemblyTests(unittest.TestCase):
    def test_unchanged_generation_preserves_build_input_timestamps(self):
        sketch = OUTPUT.parent.parent
        paths = [OUTPUT, *(sketch / name for name in ('WebUI_gz.h', 'OtaAssets_gz.h', 'SetupAssets_gz.h'))]
        before = {path: path.stat().st_mtime_ns for path in paths}
        subprocess.run([str(sketch / 'tools/gen_webui_gzip_header.sh')], check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        self.assertEqual(before, {path: path.stat().st_mtime_ns for path in paths})

    def test_real_sources_equal_served_html(self):
        self.assertEqual(assemble().encode(), OUTPUT.read_bytes())

    def test_nested_includes_preserve_order_and_whitespace(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'js').mkdir()
            (root / 'index.html').write_text('A<script><!-- @include "js/app.js" --></script>Z\n')
            (root / 'js/app.js').write_text('one\n/* @include "part.js" */three')
            (root / 'js/part.js').write_text('two\n')
            self.assertEqual(assemble(root), 'A<script>one\ntwo\nthree</script>Z\n')

    def test_missing_cycle_and_escape_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for content, error in [('<!-- @include "missing.js" -->', FileNotFoundError),
                                   ('<!-- @include "index.html" -->', ValueError),
                                   ("<!-- @include 'missing.js' -->", ValueError),
                                   ('<!-- @include "../outside.js" -->', ValueError)]:
                (root / 'index.html').write_text(content)
                with self.assertRaises(error):
                    assemble(root)
            (root / 'outside-link.js').symlink_to(root.parent / 'outside.js')
            (root / 'index.html').write_text('<!-- @include "outside-link.js" -->')
            with self.assertRaises(ValueError):
                assemble(root)


if __name__ == '__main__':
    unittest.main()
