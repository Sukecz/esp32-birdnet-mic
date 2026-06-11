## Web flasher

Statická stránka pro nahrání firmware birdnet-esp32-rtsp-mic (BirdNET-Go / BirdNET-Pi, Seeed XIAO ESP32-C6) přímo z prohlížeče pomocí ESP Web Tools.

Lokální cesta projektu na MINIPC: `/home/msminipc/Arduino/Birdnetmic`.

Aktuální připravený obraz: **firmware 1.9.3** (2026-06-11).

Poznámka k 1.9.3: obraz obsahuje doplněnou RTSP/UDP kompatibilitu pro BirdNET-Pi
(`RTP-Info`, `source`/`ssrc` a RTCP port handling). Bylo ověřeno proti čisté instalaci
`Nachtzuster/BirdNET-Pi` se streamem `/audio2` v BirdNET-Pi/UDP režimu.

Výchozí veřejný build:
- má zapnutou externí anténu na XIAO ESP32-C6,
- nemá nastavené OTA heslo,
- patří jen do důvěryhodné lokální sítě.

Firmware 1.9.2 a novější obsahuje opravu velikosti buildu pro Arduino IDE na Seeed XIAO ESP32-C6: soubor
`esp32-birdnet-mic/build_opt.h` vypíná nepoužitá C++ exception/unwind metadata a nechává zhruba
39 KB rezervu v defaultní 1.2 MB app partition bez odebrání funkcí.

Firmware je určený pro I2S mikrofon **ICS-43434** jako referenční/testovanou volbu.
Mikrofon **INMP441** byl uživatelsky potvrzený jako kompatibilní bez změn firmwaru při stejném
I2S zapojení (`SCK` -> GPIO21, `WS` -> GPIO1, `SD` -> GPIO2); viz:
https://github.com/Sukecz/esp32-birdnet-mic/discussions/25

### Struktura
- `index.html` – hlavní stránka s tlačítkem „Flash“ a instrukcemi.
- `manifest.json` – manifest pro ESP Web Tools, zapisuje jen firmware části bez NVS.
- `bootloader.bin`, `partitions.bin`, `boot_app0.bin` – systémové části pro USB web flash.
- `firmware.bin` – sloučený obraz (bootloader + partition + app) pro ESP32‑C6, pouze pro ruční full flash.
- `firmware-app.bin` – app-only obraz pro OTA aktualizaci přes Web UI zařízení.
- `ota-version.txt` – jednoduchý text s verzí OTA buildu.

Manifest záměrně nepoužívá sloučený `firmware.bin` na offsetu `0x0`, protože by při aktualizaci
přepsal NVS oblast od `0x9000` vyplněnými `0xff` byty a smazal Wi-Fi i aplikační nastavení.
ESP Web Tools navíc u nerozpoznaného zařízení bere flash jako novou instalaci a defaultně maže celý
flash. Proto je v manifestu zapnuté `new_install_prompt_erase`; při aktualizaci je nutné zvolit
zachování existujících dat.

### OTA aktualizace bez USB
1. Otevři **https://esp32mic.msmeteo.cz**.
2. Do části **OTA update without USB** napiš IP adresu zařízení, například `192.168.1.80`.
3. Klikni **Open OTA update**.
4. Na stránce zařízení vyber jednu možnost:
   - **Automatic update**: zařízení si samo stáhne nejnovější `firmware-app.bin` z webu.
   - **Upload compiled file**: ručně vybereš app-only `.bin` soubor.

Automatické stažení používá `http://esp32mic.msmeteo.cz/firmware-app.bin`. Server musí tento soubor
vydat přes obyčejné HTTP bez přesměrování na HTTPS. HTTPS/TLS se do výchozí XIAO ESP32-C6 partition
nevejde.

Na OTA stránku nenahrávej `firmware.bin`. To je USB merged image. Pro OTA patří `firmware-app.bin`.

### Jak připravit firmware.bin
Vygeneruj sloučený image, např.:
```bash
esptool.py --chip esp32c6 merge_bin \
  -o firmware.bin \
  0x0 bootloader.bin \
  0x8000 partitions.bin \
  0x10000 firmware_app.bin
```
Nebo použij výstup z `idf.py build` / `arduino-esp32` pokud produkuje merged bin přímo.

### Nasazení na server
Aktuální cesta webu na RPI je `/home/msrpi/web-projects/web-flasher`, nginx služba `esp32mic-web`
ji servíruje jako statický web.

1. Ulož soubory `index.html`, `manifest.json`, `bootloader.bin`, `partitions.bin`, `boot_app0.bin`, `firmware.bin`, `firmware-app.bin` a `ota-version.txt` do `/home/msrpi/web-projects/web-flasher`.
2. Ujisti se, že MIME typy pro `.json` a `.bin` jsou správně (`application/json`, `application/octet-stream`).
3. Otevři stránku v Chrome/Edge (desktop), povol přístup k sériovému portu, klikni „Flash“.

### GitHub release
Release lze spravovat z MINIPC přes přihlášené `gh`. Verze se bere z `web-flasher/manifest.json`;
do release patří flasher/OTA artefakty z této složky.

### Poznámky
- WebSerial funguje pouze na desktop Chromu/Edge (ne Safari, ne Firefox).
- Stránka musí běžet přes HTTPS (nebo `http://localhost` při lokálním testu).
