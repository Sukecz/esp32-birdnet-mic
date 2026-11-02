"""Compatibility view for legacy text contracts and hardware fragment fixtures.

New policy/module tests should include production headers/translation units.
This view keeps older contracts following the actual owners during extraction.
"""
def firmware_source(root):
    sketch = root / 'esp32-birdnet-mic'
    return '\n'.join((sketch / name).read_text() for name in
                     ('esp32-birdnet-mic.ino', 'AudioCapture.cpp', 'SettingsStore.cpp',
                      'StreamTypes.h', 'FirmwareDefaults.h', 'RtspService.cpp', 'AudioSettings.h'))
