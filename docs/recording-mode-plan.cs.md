# Návrh SD nahrávání v BirdNETMic

Sepsáno: **2026-10-25** podle domluvy s uživatelem.

**Stav: lokální implementace zahájena 2026-10-25 na pokyn uživatele.**
Hardware ještě není k dispozici. Uživatel následně povolil OTA na konfigurované
C6 pro prohlédnutí UI; veřejné vydání zůstává odložené.
Aktuální rozsah a zbývající ověření: [stav implementace](recording-mode-development.cs.md).

## 1. Cíl a rozsah první verze

Přidat samostatné nahrávání na microSD přímo do BirdNETMic. Jeden firmware
a jedno známé webové rozhraní budou sloužit pro živé streamování i terénní záznam.
Použitelný základ selektivně převzít z místního projektu
`esp32-audio-recorder`, který zatím zůstane zachovaný jako reference.

První verze bude mít dva oddělené režimy:

| Režim | Chování |
| --- | --- |
| **Streaming** | Současné RTSP a živý poslech v prohlížeči; bez zápisu na SD. |
| **Recording** | Samostatný zápis WAV na SD; RTSP je vypnuté. Webové ovládání a tlačítko Listen zůstávají dostupné i během záznamu pro kontrolu mikrofonu (doplnění uživatele 2026-10-25). |

Výchozí režim po aktualizaci bude **Streaming**, aby se zachovalo dnešní chování.
Souběžný **Streaming + Recording** je možnost do budoucna, až bude samostatné
nahrávání fyzicky ověřené. Není součástí první fáze.

## 2. Vybraný hardware

- **Adafruit microSD Card BFF Add-On 5683**, komunikace přes hardwarové SPI.
- První fyzické ověření na **základním XIAO ESP32-S3 s externím mikrofonem**.
- C3/C5/C6 zůstávají dalšími cíli; funkčnost SD ověřovat postupně pro konkrétní desku.
- Adafruit je konečná volba uživatele a nahrazuje dřívější doporučení Waveshare.
- Nativní čtyřbitové SDMMC není součástí tohoto zapojení ani první fáze.

Před zapojením ověřit SPI, CS a mikrofonní piny pro konkrétní desku. Adafruit má
výchozí CS na TX a umožňuje jeho změnu. Nepřebírat bez kontroly pinovou mapu
ze samostatného recorderu: současný BirdNETMic používá pro externí mikrofon jiné
zapojení a některé piny by se mohly střetnout.

Odkazy:

- [Vybraný modul na Botlandu](https://botland.cz/prislusenstvi-pro-pametove-karty/22693-microsd-karta-bff-add-on-deska-se-slotem-pro-pametovou-kartu-microsd-pro-qt-py-a-xiao-adafruit-5683.html)
- [Popis výrobce](https://www.adafruit.com/product/5683)
- [Vývody a změna CS](https://learn.adafruit.com/adafruit-microsd-card-bff/pinouts)

## 3. Hlavička UI: režim a skutečný stav

V hlavičce vždy zobrazovat **Mode: Streaming** nebo **Mode: Recording**.
Vedle režimu uvádět textový stav a barevný puntík; samotná barva nestačí.

| Příklad | Význam |
| --- | --- |
| **Mode: Streaming · zelený puntík · Streaming — 1 client** | Je připojený klient a zařízení mu průběžně odesílá audio. |
| **Mode: Streaming · neutrální puntík · Ready** | Zařízení je připravené a čeká na klienta. |
| **Mode: Streaming · chybový puntík · Error** | Například selhalo snímání mikrofonu. |
| **Mode: Recording · Ready** | Režim je vybraný, záznam ještě neběží. |
| **Mode: Recording · červený záznamový puntík · Recording** | Probíhá zápis na kartu. |
| **Mode: Recording · SD full / SD error** | Karta je plná nebo nastala chyba. |

Zelený stav streamingu odvozovat od aktivního klienta a úspěšného odesílání,
nikoli pouze od připojení k Wi-Fi. Neznamená potvrzení přehrávání či zpracování
na straně přijímače. Přesné názvy a barvy stavů doladit při návrhu UI.

## 4. Přepínání režimů a síť jsou nezávislé

**Streaming / Recording určuje činnost zařízení. Domácí Wi-Fi / vlastní AP
určuje způsob přístupu k jeho UI.**

Přepnutí režimu samo nemění Wi-Fi, nevytváří AP a nevyžaduje restart.
Na domácí síti zůstane UI dostupné na stejné adrese, pokud se nezmění síťové
připojení nebo přidělená IP.

Příklad s adresou `http://192.168.1.80/`:

1. Otevřít současné UI a přepnout Streaming → Recording.
2. Zařízení ukončí RTSP, ale zůstane na domácí Wi-Fi. Listen lze znovu spustit i během nahrávání.
3. Na stejné stránce nastavit délku a stisknout **Start recording**.
4. Záznam ukončit přes **Stop & save**; pak lze přepnout zpět na Streaming.

Přepnutí do Recording samo nezačne nahrávat. Při odchodu z aktivního záznamu
nejdřív potvrdit jeho zastavení a bezpečně uzavřít soubor. Selhání uložení musí
být viditelné; nesmí se vydávat za úspěšné dokončení.

## 5. Automatické AP a ovládání v terénu

Po zapnutí se zařízení pokusí připojit k uložené Wi-Fi. Pokud ji nenajde,
automaticky vytvoří vlastní přístupový bod, například `BirdNETMic-XXXX`.
Přesný název a čekací dobu ještě určit.

**AP zůstane zapnuté po celou dobu provozu zařízení:** během nahrávání,
po jeho dokončení i bez připojeného telefonu. Nebude mít časový limit,
po kterém by se ovládání vypnulo. Připojení a odpojení telefonu nesmí
přerušit nahrávání.

Předpokládaný postup v terénu:

1. Zapnout vypínačem nebo připojením USB.
2. Telefon připojit k Wi-Fi zařízení; router ani internet nejsou potřeba.
3. Otevřít UI, navrhovaná AP adresa je `http://192.168.4.1/`.
4. Vybrat Recording, zkontrolovat mikrofon a kartu, nastavit délku a spustit záznam.
5. Telefon lze odpojit; zařízení pokračuje samostatně.
6. Kdykoliv se znovu připojit pro kontrolu nebo **Stop & save**.

QR kód s adresou UI na krabičce je volitelný doplněk.
Automatické otevření stránky telefonem není podmínkou ovládání.
AP ovládání funguje v dosahu zařízení, nezajišťuje přístup přes internet.

Trvale dostupné plnohodnotné UI přes AP je **požadovaná nová funkce**.
Nelze ji považovat za hotovou podle dnešního provisioning portálu: v současném
firmwaru v1.35 má běžná startovací větev WiFiManager portál s limitem 180 sekund.
Při implementaci je třeba zkontrolovat i další obnovovací síťové větve.
Spotřebu se zapnutým AP změřit; úsporné vypínání Wi-Fi se v první fázi neplánuje.

## 6. Ovládání a délka nahrávání

Hlavní panel Recording má obsahovat:

- **Start recording** a **Stop & save**.
- Stav mikrofonu a karty, volné místo a případnou chybu.
- Uplynulý čas; při časovém limitu také zbývající čas.
- Celkovou délku: **Nepřetržitě** nebo **Po stanovenou dobu v hodinách**.
- Volbu **Spustit nahrávání po zapnutí** a uložení potřebných nastavení.

Například limit 6 hodin znamená automatické ukončení záznamu a uzavření souboru
po uplynutí limitu. Zařízení zůstane zapnuté a UI dostupné; nejde o automatické
fyzické odpojení napájení.

Nepřetržitý záznam běží do ručního zastavení, zaplnění karty nebo ochranného
zastavení/chyby. Automatické přepisování starších souborů není domluvené.

**Celková délka záznamu a délka jednotlivého WAV jsou dvě různé volby.**
Dělení souborů patří do pokročilého nastavení; hodinové soubory jsou navržený
výchozí bod. Šestihodinový nepřerušený záznam by tak vytvořil šest souborů.
Rozsah a krok časového limitu i nabídku délek souborů ještě určit.

Pro opakovaný terénní provoz jednou uložit Recording, délku a automatický start.
Příště stačí zařízení zapnout. Telefon slouží ke změně nastavení, kontrole nebo
předčasnému zastavení. Automatický start musí ověřit připravenost karty a mikrofonu.

## 7. Spolehlivost a technický základ

- Oddělit snímání mikrofonu od výstupu do sítě či na kartu. SD je hlavní výstup Recording; volitelný kontrolní Listen má vlastní omezenou frontu
  a nesmí brzdit SD. RTSP + Recording zůstává mimo první fázi.
- Použít vyhrazený buffer a obsluhu zápisu; čekání na SD nesmí blokovat snímání.
- Ze samostatného recorderu posoudit převzetí WAV zápisu, dělení souborů,
  checkpointů, práce s rozpracovanými soubory a jejich obnovy.
- Před OTA, restartem, přepnutím režimu a při tepelném zastavení nejdřív bezpečně
  ukončit záznam. Sladit to také se stávajícími plány a případným deep sleep.
- Ošetřit plnou/odebranou kartu, chyby a krátké zápisy i přetečení bufferů.
  Ztráty a přerušení musí být viditelné.
- Výpadek napájení nemusí umožnit uzavření WAV; zachovat rozpracovaný soubor
  a navrhnout obnovu. Neslibovat nulovou ztrátu posledních dat.
- Zvětšení OTA slotů ve v1.35 přidalo místo pro firmware ve flash, nikoli RAM.
  Paměťové nároky vyhodnotit pro každou desku zvlášť.
- Ověřit dlouhý reálný záznam, přechody souborů, latence karty, ovládání přes AP
  i domácí Wi-Fi a bezpečné zastavení. Build sám spolehlivost nahrávání neprokazuje.

## 8. Otevřené detaily před implementací

Tyto body nejsou hotová rozhodnutí:

- Přesný pinout Adafruit/mikrofon pro jednotlivé desky a potřebná změna CS.
- Parametry WAV: bitová hloubka, vzorkování a ukládání původního nebo upraveného PCM.
  Zvlášť posoudit I2S a PDM; 24bitový soubor sám nezaručuje 24bitovou přesnost.
- Kompatibilní karty, souborový systém, RAM rezerva a interval ukládání checkpointů.
- Rozsah časového limitu, jeho počítání při přerušení a případném obnovení po ochlazení.
- Názvy souborů, časová razítka bez internetu a případná metadata.
- Síťové detaily: identita/zabezpečení AP, čekání při startu, chování při ztrátě
  domácí sítě za provozu a jejím návratu. Síťové změny nesmí přerušit záznam.
- Vazba na tepelnou obnovu, stávající rozvrhy, plánované restarty a deep sleep.
- Přesné UI stavy po dokončení a při chybách; případné potvrzování přepnutí režimu.

Prohlížeč souborů, stahování nahrávek, automatické mazání, fyzické tlačítko,
nízkobateriové vypnutí a vzdálený přístup přes internet nejsou v domluveném
rozsahu první fáze. Lze je posoudit samostatně.

## 9. Navazující práce

Nejprve lokálně připravit oddělené režimy, UI a SD pipeline. Po doručení modulu
ověřit skutečný hardware a pinout, otestovat první S3 sestavu a podle výsledků
doladit otevřené detaily. Další desky kvalifikovat postupně.

Lokální implementace a OTA na konfigurované C6 pro kontrolu UI jsou povolené.
Fyzický SD test a veřejné vydání jsou odložené;
uživatel očekává doručení hardware přibližně za dva dny.
