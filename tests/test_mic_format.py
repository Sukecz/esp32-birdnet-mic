import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SKETCH = (ROOT / "esp32-birdnet-mic" / "esp32-birdnet-mic.ino").read_text(encoding="utf-8")
WEBUI_CPP = (ROOT / "esp32-birdnet-mic" / "WebUI.cpp").read_text(encoding="utf-8")
WEBUI_HTML = (ROOT / "esp32-birdnet-mic" / "webui" / "index.html").read_text(encoding="utf-8")


class MicFormatContractTest(unittest.TestCase):
    def test_firmware_formats_and_safe_default(self):
        self.assertIn("MIC_FORMAT_PHILIPS = 0", SKETCH)
        self.assertIn("MIC_FORMAT_MSB = 1", SKETCH)
        self.assertIn("DEFAULT_MIC_FORMAT = MIC_FORMAT_PHILIPS", SKETCH)
        self.assertIn("I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG", SKETCH)
        self.assertIn("I2S_STD_MSB_SLOT_DEFAULT_CONFIG", SKETCH)

    def test_setting_is_validated_and_persisted(self):
        self.assertIn('getUChar("micFormat", DEFAULT_MIC_FORMAT)', SKETCH)
        self.assertIn('putUChar("micFormat", micFormat)', SKETCH)
        self.assertIn("if (micFormat > MIC_FORMAT_MSB)", SKETCH)
        self.assertIn("if (newFormat > MIC_FORMAT_MSB) return false;", SKETCH)
        self.assertIn("micFormat = oldFormat;", SKETCH)

    def test_api_and_ui_contract(self):
        self.assertIn('"\\\"mic_format\\\":" + String(micFormat)', WEBUI_CPP)
        self.assertIn('key == "mic_format"', WEBUI_CPP)
        self.assertIn("v <= 1", WEBUI_CPP)
        self.assertIn("id='sel_mic_format'", WEBUI_HTML)
        self.assertIn("Adafruit SPH0645", WEBUI_HTML)
        self.assertIn("cannot damage the microphone", WEBUI_HTML)


if __name__ == "__main__":
    unittest.main()
