# Recording: stav lokální implementace

2026-10-25. Vývojový stav nad v1.36, bez nové veřejné verze. Uživatel výslovně
nejprve požadoval pouze lokální práci; následně výslovně povolil OTA na
konfigurované C6 pro prohlédnutí UI. SD hardware ještě není k dispozici.
Veřejná publikace zůstává odložená.

## Měření baterie — 28. 10. 2026

Volitelné **Device configuration → Enable battery monitoring** funguje v obou
režimech. Okno vysvětlí C5 s vestavěným děličem 100 kΩ + 100 kΩ nebo externí
dělič stejných hodnot na D0/A0 ostatních XIAO. Napětí a orientační procenta jsou
přímo v horním přehledu, bez prokliku. Podporované jsou Li-ion/LiPo 1S sestavy
včetně více paralelních článků (max. 4,2 V); sériové nikoli. Nastavení ani měření
nespouští automatické zastavení záznamu při nízkém napětí, to je odložené.
Zapojení, piny, omezení a API: [Battery monitoring](battery-monitoring.md).

## Co je v první implementaci

- Uložený režim Streaming / Recording; dosavadní instalace začínají ve Streaming.
  Přepnutí bez restartu a bez změny domácí Wi-Fi. Aktivní záznam vyžaduje
  potvrzení Stop & save; chyba uzavření zabrání tomuto přepnutí.
- Recording vypne RTSP včetně jeho ručního, MQTT a plánovaného zapnutí.
  **Listen lze zapnout i během nahrávání**, jak uživatel dodatečně požadoval.
  Poslech používá původní samostatnou omezenou frontu a nečeká na SD.
  Potvrzuje pouze snímání mikrofonu, nikoli správnost obsahu na kartě.
- SD cesta je zapnutá pro XIAO ESP32-C3/S3/C5/C6 přes shodné pozice headeru
  D6 (CS/TX), D8 (SCK), D9 (MISO), D10 (MOSI). Čísla GPIO mapuje konkrétní
  Arduino varianta. Typ čipu neblokuje Start; zůstávají kontroly skutečné karty,
  paměti a snímání. M5Stack AtomS3U není XIAO a nemá tento pinout.
  Zápis na fyzickou kartu ještě nebyl ověřen na žádné desce.
- V Recording jsou skryté RTSP streamy, adresy, klienti, streamové statistiky,
  RTP parametry, streamový rozvrh a související pozastavené plánované akce.
  Mikrofon/capture diagnostika, Listen, síť, čas a teplota zůstávají dostupné.
  Návrat do Streaming obnoví viditelnost bez změny uložených nastavení.
- PCM16 mono, little endian, současná vzorkovací frekvence 8–96 kHz,
  po současném gain/shift/high-pass zpracování. Nejde o původní 24bitový archiv.
  Změny nastavení během záznamu jsou přes HTTP blokované.
- Start recording / Stop & save; celková délka 0–168 hodin (0 = nepřetržitě),
  ukládá se v celých minutách. UI nabízí čtvrthodinový krok.
  Automatický start po zapnutí je samostatně uložený přepínač.
- Délka WAV je samostatně 1–120 minut, výchozí 60. Konec celkového limitu
  je počítán počtem přijatých vzorků; čas UI vychází ze zapsaného audia a nezahrnuje ochlazování.
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
  Datum a čas při platných hodinách, jinak unsynced; vždy také náhodný
  identifikátor session a pořadí segmentu.
  Kolize názvu se odmítne. Chybný či přerušený soubor se zachová.
- Při tepelné ochraně se záznam zastaví a vypne se snímání. Po
  ochlazení může pokračovat podle společného nastavení Automatically resume
  after cooling; při pokračování vznikne nový soubor se zbývajícím časovým limitem.
- HTTP změny/OTA během záznamu požadují nejprve Stop & save. Arduino OTA se
  během záznamu neobsluhuje. Explicitní MQTT restart nejprve čeká na uzavření;
  při timeoutu se restart nenaplánuje. Rozvrhy streamingu, deep sleep a periodický
  restart jsou v Recording pozastavené i po dokončení nahrávky.
- Start a výpadky Wi-Fi nově obsluhuje společná neblokující správa připojení pro oba
  režimy. Vlastní AP `BirdNETMic-XXXXXX` je po dokončení nastavení chráněné náhodným
  heslem dostupným přes Network → Direct Wi-Fi access. Fallback vzniká po 15 s offline;
  samostatný Recording má AP trvale. Připojenému telefonu AP při návratu LAN nezmizí.
  První nastavení používá vlastní stručnou mobilní stránku místo WiFiManageru.
  Výchozí režim po startu je oddělený od dočasného přepnutí v hlavním UI.

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
Výchozí zapojení výše platí pro externí modul. Sense při vybraném profilu používá
vestavěný slot s CS GPIO21; podrobnosti a zdroje jsou níže.

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

## Předchozí ověření první implementace 2026-10-25

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


## Následná lokální úprava na pokyn uživatele

Odstraněno omezení SD na základní S3; společná SPI cesta se nyní kompiluje
pro všechny čtyři podporované XIAO cíle. UI v Recording skrývá pozastavené
streamové části. Hostitelský test SD workeru používá pinové mapy všech čtyř desek.
Předchozí OTA ověření výše se vztahuje k předchozí sestavě.
Na následný pokyn uživatele bylo doplněno společné nastavení „Audio buffer size“
viditelné také v Recording a provedeno sestavení C3/S3/C5/C6 i přímé OTA na C6.
Nová SD cesta přesahuje původní malý slot C3/C6, proto limit kompilace nyní
odpovídá již používané tabulce s kapacitou 0x1F0000; žádná partition zařízení
se při tomto OTA neměnila. Původní neměnný v1.35 bridge zůstává zachovaný.

Ověřeno na C6: 39 uložených nastavení včetně zvoleného Recording zůstalo
shodných, skutečné gzip UI odpovídá lokálnímu obrazu, RTSP prvky jsou skryté,
Audio buffer size dostupné, Start není blokovaný typem C6 a Listen přijímá audio.
Fyzický zápis na SD nadále čeká na hardware. Vývojové obrazy a build log jsou
v `tmp/recording-development/all-xiao-review/`; žádné veřejné vydání neproběhlo.


## Obnova UI po neúspěšném prvním načtení

Při hlášení dlouhého Checking po OTA ukázal log dokončení Wi-Fi/I2S/UI do
přibližně 2 sekund od bootu a MQTT o sekundu později; již běžící API odpovídala
za 24–39 ms. Původní konkrétní zpožděný požadavek nebyl zachycen.

Regresní test ale reprodukoval chybu polling smyčky: prvotní refresh stahoval
zavřený log bez timeoutu a čekal na něj před dalším pokusem o načtení stavu.
Po selhání prvního status requestu tak zaseknutý log zadržoval veškeré další
refresh pokusy. Oprava načítá log pouze pro otevřený panel a přidává mu 8s
limit. Test ověřuje obnovu statusu i skutečné přerušení zaseknutého log requestu.

Po sestavení všech 4 desek a přímém OTA na C6 měření prvního načtení:
3,809 s včetně dostupnosti zařízení po rebootu, 103 ms od HTML k hlavním
hodnotám UI, bez JS chyb. 39 nastavení včetně režimu zůstalo shodných.
To ověřuje opravu a tento jeden nový start, nikoli zpětné určení přesné příčiny
původního minutového výpadku. Podklady jsou v lokálním
`tmp/recording-development/poll-recovery-review/`. Bez veřejného vydání.

## Společné ovládání režimu a Listen

Výběr Streaming/Recording je vlevo ve společném bloku, Listen vpravo a jeho
hlasitost přímo pod tlačítkem. Rozsah Listen je 0–150 % a mění pouze zesílení
v prohlížeči, nikoli PCM pro RTSP nebo soubory na SD. Nad 100 % může dojít ke
zkreslení při nedostatečné rezervě signálu.

Stav Streaming/Recording je nad Microphone ve stejné stavové buňce. Recording
rozlišuje Checking SD, Ready, OK při nahrávání, Saving, Saved a konkrétní chybu.
Při vstupu do Recording a po startu v tomto režimu proběhne asynchronní SD
kontrola ve stávajícím storage workeru; nezakládá adresář ani nahrávku.
Při skutečném Start se karta znovu připojí a ověří. Nepřipojený modul, chybějící
kartu a vadné zapojení nelze touto SPI kontrolou vzájemně rozlišit, proto společná
hláška zní `Recording: Error — SD unavailable`. Ve Streaming se SD nevyžaduje.

Host testy ověřují kontrolu bez souborů, nedostupnou kartu a následné zotavení,
odmítnutí souběžného Start, změny stavů v UI, skutečný GainNode 1.5 a rozložení
na desktopu i mobilu. Lokální podklady této iterace: `tmp/mode-controls-validation/`
a `tmp/mode-controls-build.log`. Fyzický zápis s SD hardwarem zůstává samostatnou
neověřenou částí; tato iterace je určena pro testovací C6 bez veřejného vydání.

Ověřeno na testovacím C6 po přímém OTA: všechna čtyři sestavení prošla,
40 porovnávaných nastavení včetně externí antény zůstalo shodných a obsluhované
gzip UI odpovídá sestavenému obrazu. Přepnutí do Recording skutečně vyvolalo
`Recording: Error — SD unavailable`, bez vytvoření nahrávky. Microphone zůstal
OK a Listen přijímal audio i v tomto stavu; GainNode v prohlížeči dosáhl 1.5.
Po testu byl obnoven Streaming a aktivní RTSP přenos. Živý prohlížeč neměl JS
chyby ani vodorovný přesah na mobilu. Firmware ponechává vývojové označení 1.36;
GitHub ani veřejný web/OTA se v této iteraci nepublikovaly.

## Sjednocení velikostí a kompaktní ovládání

Panel Recording používá nadpis 15 px jako ostatní sekce, základní text 13 px
místo výchozího 16 px a poznámky 12 px. Recording settings jsou trvale viditelné,
bez rozbalovacího prvku. Blok Mode/Listen má menší odsazení a mezery; na mobilu
je hlasitost v jednom řádku. Trvalá poznámka o jednom posluchači byla odstraněna,
provozní a chybové zprávy zůstávají.

Po sestavení C3/S3/C5/C6 a přímém OTA na C6 živý prohlížeč naměřil výšku
Mode/Listen 89 px místo 143 px na desktopu a 90 px místo 160 px na mobilu.
Ověřeno skutečné písmo 15/13 px, viditelná nastavení bez rozbalení, žádný mobilní
přesah a žádné JS chyby. 40 nastavení včetně externí antény zůstalo shodných;
zařízení zachovalo uživatelem zvolený režim Recording. Podklady:
`tmp/compact-recording-validation/`, build log `tmp/compact-recording-build.log`.
Bez veřejného vydání.


## Vestavěný microSD slot XIAO ESP32-S3 Sense

Pouze aplikovaný profil **Audio → S3 board → XIAO ESP32-S3 Sense** na S3
firmwaru přepne SD na CS GPIO21, SCK GPIO7, MISO GPIO8, MOSI GPIO9.
Výchozí S3/Plus a C3/C5/C6 nadále používají externí modul na D6/D8/D9/D10.
AtomS3U je pro SD odmítnut ještě před přístupem na SPI. Samotný čip S3 ani
pouhý výběr formátu PDM vestavěný slot nezapínají.

Zapojení vychází z aktuálních zdrojů výrobce:

- [Seeed SD guide](https://wiki.seeedstudio.com/xiao_esp32s3_sense_filesystem/):
  SD.begin(21), FAT32 karta do 32 GB, připojená Sense rozšiřující deska.
- [Seeed pin multiplexing](https://wiki.seeedstudio.com/xiao_esp32s3_pin_multiplexing/):
  SD GPIO21/7/8/9 a oddělený PDM mikrofon GPIO42 CLK / GPIO41 DATA.
- [ExpBoard v1.2 schematic](https://files.seeedstudio.com/wiki/SeeedStudio-XIAO-ESP32S3/res/XIAO_ESP32S3_Exp_Board_v1.2_SCH_PDF_260810.pdf):
  SD_CS je z výroby propojený přes R12 0R na IO21/USER_LED; alternativní R11
  na D2 je DNP. JP3 přivádí napětí pull-upům; neodpojovat. Starší wiki popisuje
  odpojení SD přes J3; pro provoz musí zůstat tovární propojení zapojené.
  Firmware neovládá uživatelskou LED na GPIO21, aby nerušil SD CS.
- Instalovaný Arduino-ESP32 3.3.12 potvrzuje S3 D8=7/D9=8/D10=9 a D6=43;
  SD.begin má výchozí takt 4 MHz a format_if_empty=false, shodně s použitým kódem.

Není potřeba zvláštní Sense binárka. Uložená volba desky se předává každému
Start a SD preflightu; worker dostává neměnnou kopii pinů pro konkrétní operaci.
Při změně profilu během nečinného Recording se SD ověří znovu. Během aktivní
operace je změna profilu odmítnuta před zastavením mikrofonu. Při neúspěšném
přepnutí mikrofonu se vrátí původní profil a kontrola SD se nespouští.

Vestavěný mikrofon se s SD nepřekrývá. Sense může používat i externí I2S mikrofon
na dosavadních D3/D1/D2 (případně MCLK D7); výběr SD zůstává vázaný na profil
Sense, ne na formát mikrofonu. Kamera se neinicializuje. Karty s upraveným
hardwarovým CS na D2 nejsou touto tovární konfigurací podporované.

API recording.storage rozlišuje sense_builtin, external_spi a unavailable.
UI ukazuje příslušný slot a pro Sense také FAT32 / 32 GB. Chybějící karta, modul
nebo chybná montáž zůstávají společným SD unavailable; nedochází k formátování.
Fyzická Sense není k dispozici: žádný host test ani build není důkaz skutečného
zápisu, elektrického zapojení, výkonu karty či dlouhodobé spolehlivosti.
Podklady výrobce a výsledky této iterace: tmp/sense-sd-validation/.

Ověření této iterace: hostitelská sada spustila skutečný SD worker s dočasnými WAV
soubory pro C3, S3 externí, S3 Sense, C5 a C6. SPI stub ověřuje skutečné argumenty
SPI.begin a SD.begin (včetně CS21 pro Sense, 4 MHz a vypnutého formátování).
Pro každý profil prošly preflight bez souborů, nedostupná karta, WAV metadata,
rotace, konečný počet vzorků, chyby zápisu/sync/rename, přetečení fronty,
přerušení mikrofonu, zastavení během mount a ochrana existujícího souboru.
S3 transakční test ověřil také blokaci během operace, opětovnou kontrolu po
Sense/default přepnutí a rollback bez nové kontroly při selhání mikrofonu.
Browser testy prošly pro zdrojové i gzip UI, včetně informace o Sense slotu.

Sestavení C3/S3/C5/C6 dokončeno. Přímé OTA na konfigurované C6 ověřilo shodu
naservírovaného UI, zachování 40 nastavení a externí antény, microphone OK
a nadále external_spi místo Sense routingu; volba S3 desky je na C6 skrytá.
Živý prohlížeč bez JS chyb a mobilního přesahu. Vývojová verze zůstává 1.36,
bez Git push/tag/release a bez veřejného web/OTA deploye. Skutečný Sense slot
zůstává fyzicky neověřený, nikoli označený za hardware-tested.

## Mode, hlasitost a Listen v jednom řádku

Ovládání je vlevo Mode a vpravo Volume (posuvník + procenta), následované Listen.
Na mobilu jsou textové popisky Mode/Volume skryté, jejich label vazby zůstávají.
Šířka posuvníku je pevná pro daný breakpoint; procenta mají pevnou pětiznakovou
šířku, nezalamují se a používají tabulární číslice. Tím při změně 99 na 100 %
nevzniká přerozdělení místa přes flex a pohyb posuvníku. Prázdné provozní/chybové
hlášky netvoří další řádek; neprázdné zprávy se mohou zobrazit pod ovládáním.

Ověřeno v host prohlížeči i na skutečném C6 pro 1280, 390 a 320 px: pořadí,
společný řádek, žádný vodorovný přesah, stejné bounding boxy posuvníku i Listen
pro 0/9/10/71/99/100/149/150 % i při tažení myší. Výška bloku na zařízení je
63 px desktop a 54 px mobil. Bez JS chyb. Všechna čtyři sestavení prošla,
přímé C6 OTA zachovalo 40 nastavení včetně externí antény a microphone OK.
Podklady: tmp/listen-inline-validation/ a tmp/listen-inline-build.log.
Vývojová 1.36; bez veřejného vydání.

## Tepelná obnova, odhad místa a datum v názvech

Recording nyní sdílí stávající tepelnou politiku a nastavení se Streaming:
stejný limit, tři potvrzující měření, okamžité nouzové vypnutí, pět souvislých
minut alespoň 10 °C pod limitem, kontrolu čerstvosti senzoru a třetí odstavení
za 24 hodin s ručním potvrzením. Auto resume musí být zapnuté ve stávajícím
nastavení; implementace uloženou volbu nemění. Opravena byla podmínka vyvolání
tepelné ochrany: zahrnuje i capture v Recording, které má RTSP vypnuté.
Streamovací rozvrh neblokuje tepelnou obnovu samostatného Recordingu.

Při přehřátí se nejprve dopíše fronta a uzavře soubor, pak vypne capture.
Jen úspěšně uložený přerušený záznam dostane příznak pro pokračování.
Po ochlazení vznikne nová session/soubor. Původní délka se neobnovuje od nuly:
doběhne zbývající zvukový čas; chlazení se do něj nepočítá. Zachová se souhrn
zapsaných dat a délky. Chyba SD nebo capture se automatickým pokusem nezakrývá.
Ruční Stop i odchod do Streaming pokračování zruší. Při opakovaném odstavení
lze po splnění ochlazení pokračovat ručním Acknowledge; po ručním Stop už nikoli.
Recording settings jsou během čekání na pokračování zamčené až do Stop.
Kontext zbývající nahrávky je zatím v RAM: přežití výpadku napájení/restartu
zůstává samostatnou neimplementovanou funkcí. Obvyklý start s uloženým
Recording + auto-start nadále funguje podle dřívějších pravidel.

API přidává cooling_pending a space_seconds. Odhad vychází z posledního
zjištěného počtu volných bajtů / (aktuální vzorkování × 2 pro PCM16 mono).
UI ho označuje jako přibližný a upozorní, pokud je menší než zbývající plánovaná
délka. Režie souborového systému a WAV ani budoucí chyby karty nejsou slibovanou
kapacitou. Před úspěšnou kontrolou SD je odhad nedostupný; při zápisu se volné
místo obnovuje s checkpointy každých 5 sekund.

Názvy souborů používají místní datum a čas podle nastavené časové zóny:
YYYY-MM-DD_HH-MM-SS_<session>-<segment>.wav. Unikátní identifikátor a pořadí
chrání před kolizí i při posunu času. Bez platného času (stejná hranice jako
zbytek firmwaru: po 2023-01-01) je prefix unsynced. .part a ochrana před
přepsáním zůstávají zachované. AP/startovací větve se v této úpravě nemění.
Zprávy Listen jako Listening stopped. jsou vpravo pod tlačítkem Listen.

Ověřeno hostitelsky: skutečný SD worker pro C3, S3 externí, S3 Sense, C5 a C6,
uložení před chlazením, nový soubor po navázání, přesný původní limit zvukových
vzorků bez započtení pětiminutové pauzy, pokračování při změně 8→16 kHz,
rušení pokračování a chyba karty po obnovení. Názvy ověřeny s pevným UTC časem
v Europe/Prague i bez času; odhad kapacity kontrolován proti PCM datovému toku.
Skutečné tepelné funkce prošly scénáři Recording bez RTSP, vypnutý streamovací
rozvrh, vypnuté automatické navázání, ruční Stop, třetí odstavení, chyby senzoru,
nouzové odstavení, ruční potvrzení a chyby obnovení capture. Dosavadní Streaming
a OTA testy prošly také. UI testy ověřily odhad, varování kapacity, blokaci
nastavení při chlazení a zprávu pod pravým tlačítkem Listen.

Všechna čtyři finální sestavení dokončena, poté přímé OTA na C6. Zachováno
40 nastavení včetně externí antény. Mikrofon OK, naservírované UI shodné
s vloženou gzip verzí, mobil bez přesahu a bez JS chyb. Živý prohlížeč ověřil
zprávu pod Listen na 1280/390/320 px. Na C6 zůstala zapnutá automatická obnova,
limit 85 °C a cílové ochlazení 75 °C; nebyl prováděn fyzický test přehřátím ani
zápis na skutečnou SD. Podklady: tmp/recording-enhancements-validation/,
finální build log tmp/recording-enhancements-final-build.log.
Bez veřejného vydání; fyzická SD kvalifikace zůstává otevřená.

## Sjednocené první nastavení a Wi-Fi fallback (2026-10-26)

Lokálně doplněn [plán a výsledky integrace](device-setup-integration-plan.cs.md):
krátká mobilní stránka místo WiFiManageru, profily desek, C6 anténa, výchozí režim
oddělený od aktuálního a domácí/samostatná Wi-Fi s automatickým fallbackem.
Nové přihlašovací údaje se nejdříve ověří bez přepsání původních.
Na C6 byla ověřena migrace 40 nastavení, chybné připojení a návrat do původní sítě,
aktivní chráněné AP po chybě a návrat do výchozího Recording po restartu.
Externí anténa a funkční mikrofon zachovány. Všechny čtyři obrazy sestaveny;
bez GitHub push, veřejného releasu nebo veřejného OTA nasazení.
Fyzické SD a Sense ověření zůstává otevřené.

## Oprava mobilního výběru Wi-Fi a AP hesla (2026-10-26)

Na testovacím S3 původní scan vracel sítě, ale výsledky byly pouze ve skryté HTML
nápovědě datalist. Průvodce nyní vykresluje viditelný seznam tlačítek sítí, deduplikuje
SSID a po klepnutí vyplní jeho název. První setup používá běžnou dobu scanu 300 ms/kanál.
Nová AP hesla mají 8 čitelných znaků. Stará dokončená nastavení se při OTA nemění;
u této testovací S3 bylo na výslovné přání heslo změněno přes chráněnou akci na 8 znaků.

Prošlo 22 hostitelských testů, mobilní testy čitelných i minifikovaných zdrojů a build
C3/S3/C5/C6. Na skutečném S3 po lokálním OTA prohlížeč našel 3 sítí, ověřil
viditelný seznam, vyplnění SSID klepnutím a osmiznakové heslo přes Network → Direct Wi-Fi access.
Ostatní nastavení zachovaná, bez JS chyb. Verze zůstává 1.36 a nebyla publikována.
SHA-256 S3: `00fa6b0595032b16411e0eb8a621def1e4150084d883d5b437d768a20aff63e9`. Důkazy: `tmp/setup-scan-fix/`.

### Zpřístupnění konfigurace a Device Wi-Fi (2026-10-26)

- Otevření Audio automaticky otevře Advanced audio settings; uživatel je může ručně sbalit.
- Device configuration je v horní navigaci. Zavedená instalace používá Save changes a stručný úvod Board, startup mode and Wi-Fi. Bez dodatečné poznámky o zachování nastavení.
- Network obsahuje přímo viditelný blok Device Wi-Fi se stavem Active/Standby, názvem sítě, heslem a adresou ovládání. Heslo se načte chráněným POST při otevření Network.
- All-board build C3/S3/C5/C6 a testy setup/native, setup/browser, browser audio, recording UI a thermal UI prošly.
- Lokální OTA C6 192.168.1.80: SHA-256 `512353dd17146904bb258d860488bc615b82cc1529ab1721b219995cb0288aff`, 1324944 B. Zachováno 40 kontrolovaných parametrů, externí anténa, audio producer a mikrofon OK; servírované UI odpovídá sestavení.
- Živý prohlížeč na C6: 320/390/1280 px, automatické otevření i ruční sbalení Advanced audio settings, viditelné údaje Device Wi-Fi, konfigurace a Save changes, bez vodorovného přetečení a JS chyb. Důkazy v lokálním `tmp/ui-network-refinement/`.
- Bez Git push, veřejného nasazení nebo aktualizace S3.

### Uzavření dne 2026-10-26

Aktuální souhrn všech nevydaných změn, rozdíl mezi firmwarem na C6/S3 a otevřené
ověřování je v [předávacím zápisu](unreleased-progress.cs.md). Poslední C6 OTA už
obsahuje vizuálně oddělený panel Device Wi-Fi; SHA-256
`2dc4014126194c5efc8e037b71828eaaf4402c09a350d6cbbd522dba8a3e7d3f`.
Všechny čtyři varianty sestaveny, zachováno 40 parametrů, externí anténa a mikrofon OK.
Prohlížeč prošel na 320/390/1280 px; důkazy `tmp/device-wifi-panel/`.
Bez Git push a veřejného vydání. Pro dnešek práce ukončena.

## Lokální opravy SD auditu (2026-10-29)

- Chyba čtení I2S okamžitě zastaví přijímání dat do nahrávky; nevznikne zdánlivě úspěšný záznam po krátkém výpadku.
- Stop zachovává chybový stav při souběhu s úklidem SD. Úspěšné pozdní dokončení vymaže předchozí timeout.
- UI porovnává délku v celých minutách, varuje před opuštěním neuložených Recording voleb a při chybě pollingu nepotvrzuje Recording OK.
- Check SD card opakuje kontrolu bez vytváření nahrávky; při nahrávání je vypnutý. Wi-Fi scan je během nahrávání blokovaný.
- Všechny podporované XIAO (C3/S3/C5/C6) používají adaptivní interní SD frontu 32–128 KiB podle volné RAM. Před alokací se ponechává 72 KiB pro storage task, síť a UI; při fragmentaci se zkoušejí menší bloky. PSRAM není podmínkou. M5Stack AtomS3U SD nepodporuje.
- Maximální prodleva zahrnuje i checkpoint a rotaci souborů. Fyzickou spolehlivost karet musí potvrdit test s adaptérem.

Ověřeno: host testy Recording, lifecycle/recovery, stream health, thermal recovery, setup a web assets; browser testy Recording, poll recovery a setup. Sestaveny C3/S3/C5/C6. Lokální OTA C6 zachovalo 40 porovnávaných nastavení a externí anténu; capture OK, I2S errors 0, NTP potvrzen. Živé mobilní UI bez JS chyb a přesahu; Check SD card správně hlásí nedostupnou kartu, po testu obnoven Streaming. Důkazy: tmp/sd-audit-validation a tmp/sd-audit-fixes-build.log. Bez GitHub push a veřejného nasazení.


## Společná SD SPI oprava a A/B (29. 10. 2026)

Na C5 s Adafruit microSD BFF a 32GB SDHC byly opakovaně zachyceny I/O chyby
včetně částečných zápisů. Změna frekvence sama nestačila: větší zápisy při
16 MHz prošly krátkými testy, ale záznam přes UI selhal po 124 s. Zápis po
512 B při 16 MHz prošel 305 s, byl však přibližně čtyřikrát pomalejší.

Srovnání SPI ovladačů odhalilo odlišné čekání na kartu: Arduino SD přijímalo
libovolný nenulový bajt místo stabilního 0xFF a chyběl priming bajt po CS LOW.
Společný patch také doplňuje release clocks po CS HIGH. Opravou prošly celé
transportní testy při 4/16/20 MHz i 304s skutečný UI záznam, bez nových SD,
I2S/DMA chyb a ztracených vzorků. Během transportních testů se porovnává každý
bajt známého patternu po zavření a novém otevření. U WAV se před přejmenováním
ověří hlavička a fyzická délka; to není úplné porovnání mikrofonního PCM.

`tools/prepare_sd_library.py` připraví z SHA-připnuté Arduino SD 3.3.12 privátní
kopii v build/sd-compat. Instalovaný core nemění. `tools/compile_firmware.sh`
stejnou knihovnu přilinkuje ke všem C3/S3/C5/C6, včetně profilů S3 Sense.
Výchozí SPI 16 MHz a blok do 4096 B jsou společné; GPIO mapování zůstává podle
desky. Fronta se řídí skutečnou pamětí: 512KiB PSRAM, 128KiB menší PSRAM,
nebo adaptivních 32–128 KiB interně. PSRAM fronta zachová interní rezervu
64 KiB, interní fronta 72 KiB; fallback po fragmentované PSRAM limit nesnižuje.
Uložené Settings/NVS zůstávají stejné.

Stav doplňuje kartu/FAT/cluster, kapacitu fronty, první I/O chybu (operace,
errno, offset, požadovaný/skutečný počet bajtů), čas i neúspěšného zápisu
a explicitní diagnostiku s novým exkluzivním souborem 1 MiB + 44 B.
`action=clock&hz=16000000&block_bytes=4096` mění jen runtime parametry,
`action=diagnose` spouští zápis/read-back; akce používají stávající CSRF ochranu.
Starší záznamy se nemažou. Důkazy a hostitelské testy:
`tmp/sd-write-investigation/`. Bez změny verze a veřejné publikace.

Fyzický SD test je zatím na vložené 32GB kartě v C5. Ostatní desky mají
kompilaci, společný patch a modelové testy včetně běhu bez PSRAM; to není
fyzická kvalifikace SD karty na těchto deskách. C6 prošla dočasným přepnutím
režimů a alokačním preflightem na starším image, pak obnovena na Streaming
s původní External anténou; nemá doložený SD modul.

Finální společný image prošel ještě 183s skutečným UI záznamem na C5 (17 205 KiB
PCM, max. zápis 205 ms), bez nových I/O, DMA/I2S chyb a ztracených vzorků.
Start a Stop & save přes UI fungují, hlavička a délka WAV ověřena po otevření.
Před/po poslední OTA zachováno 47 nastavení včetně aktuálního časového pásma
a C5 korekce baterie; NTP i UTC/lokální čas ověřeny. Fyzické dlouhodobé testy
se všemi deskami/kartami zůstávají odlišné od těchto krátkých ověření.

## Logování nahrávání a noční test (lokální vývoj 29. 10. 2026)

Recording posílá začátek/pokračování, otevření a ověřené uložení jednotlivých
segmentů, požadavek na Stop, konec s důvodem a počtem skutečně zapsaných bajtů,
chyby včetně prvního I/O selhání a průběžný souhrn každých pět minut do
společného UI/serial logu. Zachycuje i automatické konce podle délky, SD chyby,
pauzu při chlazení a diagnostiku. Dříve se tyto stavy měnily pouze v UI/API.

SD worker do sdíleného String bufferu nepíše: kopíruje pevné události do
omezené fronty pod krátkým zámkem. Hlavní smyčka teprve sestaví textové zprávy.
Zvukový producer a jeho push cesta neformátují logy ani nečekají na síť/SD.
Při zaplnění fronty se explicitně ohlásí počet zahozených starších událostí.
Opakované Stop nezahltí log a nemůže přepsat již uložený stav Saved při
souběhu s dokončováním workeru. Před hlubokým spánkem se finální události
v hlavní smyčce vyberou.

Log v zařízení zůstává RAM bufferem posledních 120 řádků a restart jej smaže.
Pro dlouhý test proto lokální `scripts/monitor_recording_run.py` archivuje
čtením API log, stav Recording, počet ztracených vzorků, DMA/I2S čítače,
paměť a teplotu. Skript nemá žádné zapisovací volání do zařízení a nahrávání
nespouští ani nevypíná. Při vypnutí Wi-Fi se archivace přeruší; SD záznam
zůstává samostatný.

Hostitelské testy provádějí skutečný worker pro všechny čtyři rodiny a S3 Sense:
pořadí/zprávy, přesné bajty, rotaci, konec podle délky, ruční stop, původní
chybu při různých I/O selháních, průběžný log a přeplnění fronty.
Noční běh na C5 se hodnotí odděleně po jeho dokončení; krátký test není důkaz
stability přes noc ani všech podporovaných fyzických kombinací.
