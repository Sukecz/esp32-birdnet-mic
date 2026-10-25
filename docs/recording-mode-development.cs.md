# Recording: stav lokální implementace

2026-10-25. Vývojový stav nad v1.36, bez nové veřejné verze. Uživatel výslovně
nejprve požadoval pouze lokální práci; následně výslovně povolil OTA na
konfigurované C6 pro prohlédnutí UI. SD hardware ještě není k dispozici.
Veřejná publikace zůstává odložená.

## Co je v první implementaci

- Uložený režim Streaming / Recording; dosavadní instalace začínají ve Streaming.
  Přepnutí bez restartu a bez změny domácí Wi-Fi. Aktivní záznam vyžaduje
  potvrzení Stop & save; chyba uzavření zabrání tomuto přepnutí.
- Recording vypne RTSP včetně jeho ručního, MQTT a plánovaného zapnutí.
  **Listen lze zapnout i během nahrávání**, jak uživatel dodatečně požadoval.
  Poslech používá původní samostatnou omezenou frontu a nečeká na SD.
  Potvrzuje pouze snímání mikrofonu, nikoli správnost obsahu na kartě.
- První SD profil je pouze **základní XIAO ESP32-S3 s externím mikrofonem**.
  Sense/Atom a C3/C5/C6 dovolují prohlédnutí režimu a jeho UI, ale Start recording
  zůstává zakázaný; SD firmware se pro C3/C5/C6 nekompiluje. Zůstávají cíli buildu
  pro kontrolu zachování streamingu. Žádný SD profil není fyzicky kvalifikovaný.
- PCM16 mono, little endian, současná vzorkovací frekvence 8–96 kHz,
  po současném gain/shift/high-pass zpracování. Nejde o původní 24bitový archiv.
  Změny nastavení během záznamu jsou přes HTTP blokované.
- Start recording / Stop & save; celková délka 0–168 hodin (0 = nepřetržitě),
  ukládá se v celých minutách. UI nabízí čtvrthodinový krok.
  Automatický start po zapnutí je samostatně uložený přepínač.
- Délka WAV je samostatně 1–120 minut, výchozí 60. Konec celkového limitu
  je počítán počtem přijatých vzorků; čas UI je monotónní běhový čas.
- Samostatný SD worker, neblokující 32 KiB PCM fronta, zásobník workeru 6 KiB.
  Tyto prostředky se alokují při prvním startu a zůstávají do restartu;
  před alokací se kontroluje rezerva v interní RAM. SPI se po uzavření uvolní.
- Zápis přes SPI 4 MHz, žádné formátování a žádné automatické mazání.
  Krátký zápis, checkpoint/fsync/close/rename chyba, přetečení fronty,
  přerušení snímání nebo DMA overrun zastaví záznam s viditelnou chybou.
- Kontrola volného místa při připojení a checkpointu; aktualizace hlavičky WAV
  a kontrolovaný `fsync` každých 5 sekund. Údaje o kartě jsou poslední měření,
  nikoliv průběžná detekce vytažení karty v klidu.
- Aktivní soubor `.wav.part` se přejmenuje na `.wav` až po úspěšném uzavření.
  Náhodný identifikátor session a pořadí segmentu; bez závislosti na NTP.
  Kolize názvu se odmítne. Chybný či přerušený soubor se zachová.
- Při tepelné ochraně se záznam zastaví a vypne se snímání. Nový záznam po
  ochlazení vyžaduje ruční Start; automatické navázání recording zatím není hotové.
- HTTP změny/OTA během záznamu požadují nejprve Stop & save. Arduino OTA se
  během záznamu neobsluhuje. Explicitní MQTT restart nejprve čeká na uzavření;
  při timeoutu se restart nenaplánuje. Rozvrhy streamingu, deep sleep a periodický
  restart jsou v Recording pozastavené i po dokončení nahrávky.
- Při startu **s uloženým Recording** se čeká na domácí Wi-Fi do 15 sekund;
  při nedostupnosti vznikne otevřené recovery AP `ESP32-RTSP-Mic-AP` s plným UI
  na obvyklé adrese `192.168.4.1`. Jakmile se v Recording recovery AP vytvoří,
  zůstává do vypnutí i po návratu domácí Wi-Fi a odpojení telefonu.
  Nová instalace ve Streaming zatím používá původní WiFiManager provisioning;
  sjednocení všech startovacích větví je další krok, ne dokončená část plánu.

## Zapojení připravené pro bench

Zkontrolováno proti pinové mapě instalovaného Arduino-ESP32 3.3.12.

| Signál | XIAO ESP32-S3 | GPIO |
| --- | --- | --- |
| SD CS | D6 / TX | 43 |
| SD SCK | D8 | 7 |
| SD MISO | D9 | 8 |
| SD MOSI | D10 | 9 |
| Mikrofon BCLK | D3 | 4 |
| Mikrofon WS | D1 | 2 |
| Mikrofon DATA | D2 | 3 |
| Mikrofon MCLK, pokud použit | D7 | 44 |

CS používá [výchozí TX propojku Adafruit 5683](https://learn.adafruit.com/adafruit-microsd-card-bff/pinouts).
Nepřepojovat CS podle původního samostatného recorderu: jeho D3 je v BirdNETMic
obsazené mikrofonem. Fyzické zapojení, napájení a skutečný modul ověřit před OTA.
Nejde o podporu libovolného SD modulu ani S3 Sense slotu.

## Obnova přerušeného souboru mimo zařízení

Původní `.part` soubor zachovat a nejprve zkopírovat z karty. Potom na počítači:

```sh
python3 tools/recover_recording.py input.wav.part recovered.wav
```

Nástroj vytvoří nový soubor, opraví délku podle dostupných celých PCM vzorků
a odmítne přepsat existující výstup. Nedokáže rekonstruovat ztracené vzorky,
odhalit všechny chyby karty či zaručit obnovu při poškození filesystemu.
Na zařízení se rozpracované soubory zatím automaticky neopravují.

## Lokální ověření a co následuje

Host testy používají skutečný SD worker a dočasné soubory s injektovanými chybami:
krátký zápis, plná/nedostupná karta, fsync/rename, Stop během mountu,
přetečení fronty, chyba snímání, časový limit a přesné dělení souborů.
Samostatně se ověřuje WAV hlavička, recovery, UI a stálost field AP.
Browser test kontroluje hlavní ovládání, chyby uložení, CSRF a mobilní zobrazení.

To nenahrazuje bench. Po doručení hardware ověřit:

1. Kartu, napájení, pinout, FAT filesystem, skutečné WAV parametry a přehrání.
2. Dlouhý záznam a rotaci; SD latence, zbývající RAM a DMA chyby při zapnutém Listen.
3. Vytažení/plnou kartu, přerušení napájení, zachování `.part` a offline obnovu.
4. Start v terénu, stálé AP, opakované připojení telefonu a návrat domácí Wi-Fi.
5. Start/Stop, přepnutí režimů, limit, autostart, tepelný stop a ochrany OTA/restartu.
6. Teprve podle výsledků řešit automatické navázání po ochlazení, metadata/časové
   názvy, obnovu na zařízení, další profily a veřejné vydání.

## Výsledek ověření 2026-10-25

- Build C3/S3/C5/C6 úspěšný, manifest validní. Bez změny verze či partition layoutu.
- 30 souvisejících hostitelských testů úspěšných; samostatně prošly WAV/field-AP
  C++ testy a nové i původní browser testy UI.
- Opraveno a regresně pokryto opakované rušení Listen ve větvi vypnutého RTSP.
- Na výslovný dodatečný pokyn uživatele proběhlo přímé OTA na konfigurovanou C6.
  Identita před OTA ověřena, 35 nastavení po OTA shodných, gzip UI přesně odpovídá
  lokálnímu sestavení. Vývojová sestava nadále hlásí v1.36.
- Skutečný browser na C6: Recording panel dostupný, Start pro nepodporované SD
  zakázaný, Listen přijímá audio a mobilní layout nepřetéká.
  Další síťová kontrola přijala 192 512 bajtů PCM za 2 sekundy při 48 kHz.
  To dokládá příjem PCM, ne subjektivní kvalitu poslechu ani zápis na SD.
- Zařízení vráceno do Streaming, RTSP a snímání běží, čítač I2S chyb je nula.
- Vývojové app-only obrazy, build log a snímky UI jsou odděleně v lokálním
  ignorovaném `tmp/recording-development/`. Tracked binárky veřejného v1.36
  byly po buildu obnovené; neproběhl push, tag, release ani veřejný OTA deploy.
