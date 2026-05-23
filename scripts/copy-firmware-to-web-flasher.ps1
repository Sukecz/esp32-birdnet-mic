# Copy PlatformIO build artifacts into web-flasher/firmware for the browser flasher
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root ".pio\build\esp32-c3-super-mini"
$dest = Join-Path $root "web-flasher\firmware"
$bootApp0 = Join-Path $env:USERPROFILE ".platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin"

if (-not (Test-Path "$build\firmware.bin")) {
    Write-Host "Build first: pio run -e esp32-c3-super-mini"
    exit 1
}

New-Item -ItemType Directory -Force -Path $dest | Out-Null
Copy-Item "$build\bootloader.bin" $dest -Force
Copy-Item "$build\partitions.bin" $dest -Force
Copy-Item "$build\firmware.bin" $dest -Force
Copy-Item $bootApp0 (Join-Path $dest "boot_app0.bin") -Force
Write-Host "Copied firmware binaries to $dest"
