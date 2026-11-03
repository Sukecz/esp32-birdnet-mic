"""Use an installed project-local toolchain without changing other projects."""
import os
from pathlib import Path

def activate():
    root = Path(__file__).resolve().parents[1]
    if (root / 'tmp/arduino-data/packages/esp32/hardware/esp32').is_dir():
        os.environ.setdefault('ARDUINO_DIRECTORIES_DATA', str(root / 'tmp/arduino-data'))
        os.environ.setdefault('ARDUINO_DIRECTORIES_DOWNLOADS', str(root / 'tmp/arduino-downloads'))
