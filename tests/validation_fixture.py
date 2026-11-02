from pathlib import Path

def uint_parser():
    header = Path(__file__).resolve().parents[1] / 'esp32-birdnet-mic/SettingsValidation.h'
    return f'\n#include "{header}"\nusing settingsvalidation::parseUInt32Strict;\n'
