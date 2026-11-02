"""Legacy HTTP fragment fixtures follow the real OTA translation unit."""
def webui_source(root):
    sketch = root / 'esp32-birdnet-mic'
    return (sketch / 'WebUI.cpp').read_text().replace(
        '// OTA service implementation is owned by OtaService.cpp.',
        (sketch / 'OtaService.cpp').read_text())
