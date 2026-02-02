/* ==========================================================================
 * PROJEKT: Inteligentní monitoring kotelny (ESP32)
 * ==========================================================================
 * * SCHÉMA ZAPOJENÍ PINŮ (Pinout):
 * * [ TYP SBĚRNICE ]    [ PIN ]    [ POPIS ZAPOJENÍ ]
 * --------------------------------------------------------------------------
 * Napájení               microUSB   Vstupní napájení (z USB nebo zdroje)
 * * OneWire (Digitální)  GPIO 4     Teplotní čidla DS18B20 (7x)
 *                        3.3V       VCC  (Napájení modulu)
 *                        GND        GND  (Zem modulu)
 * - Vyžaduje pull-up rezistor 4.7kΩ k 3.3V, 
 * - zajištěno přes DS18B20 adaptér pro teplotní sondu - Arduino
 * * SPI (MAX6675)        GPIO 18    SCK  (Clock - Hodiny)
 *                        GPIO 19    SO   (MISO - Data z modulu)
 *                        GPIO 5     CS   (Chip Select - Výběr čipu)
 *                        3.3V       VCC  (Napájení modulu)
 *                        GND        GND  (Zem modulu)
 * * Možnosti pro rozšíření:
 * * Displej
 * * I2C (Displej/jiné)   GPIO 21    SDA  (Datová linka - pokud je použit OLED)
 *                        GPIO 22    SCL  (Hodinová linka)
 * * Regulace vnetilátoru:
 * * AC Dimmer (Atmos)    GPIO 12    PWM  (Řízení výkonu ventilátoru)
 *                        GPIO 14    Z-C  (Zero Cross - detekce průchodu nulou)
 * --------------------------------------------------------------------------
 * POZNÁMKY:
 * - Pro termočlánek spalin (MAX6675) je u delších tras vhodné použít 
 * stíněný kabel nebo kondenzátor 100nF přímo na svorkách modulu.
 * - Čidla DS18B20 jsou zapojena v paralele (hvězda/sběrnice).
 * ==========================================================================
 * SOFTWAROVÉ SPECIFIKACE A LOKÁLNÍ ÚLOŽIŠTĚ:
 * ==========================================================================
 * * POUŽITÉ KNIHOVNY (PlatformIO lib_deps):
 * - milburton/DallasTemperature @ ^3.9.1
 * - paulstoffregen/OneWire @ ^2.3.8
 * - adafruit/MAX6675 library @ ^1.1.2
 * - LittleFS (Souborový systém pro ESP32 Flash)
 * * * FORMÁT DAT (CSV):
 * - Soubory: /history.csv (aktuální), /history_old.csv (předchozí období)
 * - Vzorkování: 1x za minutu.
 * - Struktura řádku: "ČAS",S1,S2,S3,S4,S5,S6,S7,SPALINY
 * - (S1-S6: Kotel/Aku, S7: Venkovní, SPALINY: Termočlánek)
 * * * INTELIGENTNÍ SPRÁVA PAMĚTI (Rotace):
 * - Systém automaticky hlídá velikost souboru (maxRecords = 2880 řádků = 48h).
 * - Při zaplnění se 'history.csv' přejmenuje na 'history_old.csv' (původní archiv se smaže).
 * - Celková dostupná historie na webu: cca 96 hodin (4 dny) ve dvou souborech.
 * - Webové rozhraní streamuje data po částech (prevence vyčerpání RAM ESP32).
 * * * GRAFY (Chart.js):
 * - Pro zachování plynulosti vykresluje graf každý 10. záznam (10min interval).
 * - Tabulka v /list_page zobrazuje kompletní nezpracovaná data z obou souborů.
 * ==========================================================================
 * AUTOR: Tomáš Zdráhala (zdrahat@gmail.com)
 * DATUM: 31.1.2026
 * ==========================================================================
 */

#include <Arduino.h> // Základní knihovna Arduino
#include <WiFi.h> // Knihovna pro WiFi připojení
#include <WebServer.h> // Knihovna pro webový server
#include <HTTPUpdateServer.h> // Knihovna pro HTTP aktualizace
#include <LittleFS.h> // Knihovna pro souborový systém LittleFS
#include <OneWire.h> // Knihovna pro 1-Wire komunikaci
#include <DallasTemperature.h> // Knihovna pro Dallas teplotní čidla
#include <ArduinoOTA.h> // Knihovna pro OTA aktualizace
#include "max6675.h" // Knihovna pro MAX6675 teploměr
#include "time.h" // Knihovna pro časové funkce

// --- KONFIGURACE SÍTĚ ---
const char* ssid = "zdrahala_Mikrotik";
const char* password = "mojewifi65";

// --- NASTAVENÍ ČASU (NTP) ---
const char* ntpServer = "pool.ntp.org"; // NTP server
const long  gmtOffset_sec = 3600;      // UTC+1 (střední Evropa)
const int   daylightOffset_sec = 3600; // Letní čas +1h

// --- NASTAVENÍ LOGOVÁNÍ ---
const char* filename = "/history.csv"; 
const char* oldFilename = "/history_old.csv"; // Záložní soubor
unsigned long lastLogTime = 0; 
const unsigned long logInterval = 60000; // 1 minuta
const int maxRecords = 2880;             // Po kolika řádcích soubor odsunout (cca 48h při 1 min)

// --- KOREKCE ČIDEL ---
const float spalinyOffset = 0.0; // Hodnota korekce pro termočlánek (např. -8.0 stupňů)

// --- KONFIGURACE ČIDEL (ADRESA + NÁZEV) ---
struct SensorConfig {
  DeviceAddress adr;
  String name;
  float offset; // Přidáme sloupec pro korekci
};

// Mapování s přidanou korekcí (offset)
SensorConfig mojeCidla[] = {
  {{ 0x28, 0x40, 0x43, 0x0c, 0x50, 0x25, 0x06, 0x46 }, "Vstup kotle",  0.0}, // S1
  {{ 0x28, 0xcc, 0xf7, 0x88, 0x43, 0x25, 0x06, 0xf8 }, "Výstup kotle", 5.0}, // S2, korekce 5,0°C
  {{ 0x28, 0xda, 0x01, 0xf4, 0x43, 0x25, 0x06, 0x91 }, "Aku - Horní",  0.0}, // S3
  {{ 0x28, 0x66, 0x58, 0xfa, 0x42, 0x25, 0x06, 0x33 }, "Aku - Střed 1", 0.0}, // S4
  {{ 0x28, 0x76, 0x9f, 0xbc, 0x43, 0x25, 0x06, 0x59 }, "Aku - Střed 2", 0.0}, // S5
  {{ 0x28, 0x15, 0x0e, 0xe4, 0x43, 0x25, 0x06, 0x7d }, "Aku - Dolní",  0.0}, // S6
  {{ 0x28, 0xbb, 0x8a, 0x10, 0x43, 0x25, 0x06, 0x99 }, "Venkovní",     0.0}  // S7
};

// --- GLOBÁLNÍ PROMĚNNÉ A OBJEKTY ---
bool isWriting = false; // Indikátor zápisu do souboru
WebServer server(80); // Webový server na portu 80
HTTPUpdateServer httpUpdater; // Objekt pro HTTP aktualizace
OneWire oneWire(4); // OneWire na GPIO4
DallasTemperature sensors(&oneWire); // Dallas teplotní čidla
MAX6675 thermocouple(18, 5, 19); // MAX6675 na SCK=18, CS=5, MISO=19

// Funkce pro získání času
String getTimestamp() {
  struct tm timeinfo; // Struktura pro časové informace
  if (!getLocalTime(&timeinfo)) return "00:00"; // Pokud nelze získat čas, vrať 00:00
  char timeStringBuff[10]; // Buffer pro formátovaný čas
  strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M", &timeinfo); // Formátování času
  return String(timeStringBuff); // Vrácení formátovaného času jako String
}

// Ořezání souboru na maximální počet záznamů
void rotateFiles() {
  isWriting = true;
  if (LittleFS.exists(oldFilename)) LittleFS.remove(oldFilename); // Smazat historii
  LittleFS.rename(filename, oldFilename); // Aktuální se stane historií
  isWriting = false;
  Serial.println("Provedena rotace souborů.");
}

// Logování dat
void logData(float t[], float spal) {
  int lineCount = 0;
  // 1. Zjistíme aktuální počet řádků (pokud soubor existuje)
  if (LittleFS.exists(filename)) {
    File countFile = LittleFS.open(filename, FILE_READ);
    while(countFile.available()) { 
      if(countFile.read() == '\n') lineCount++; 
    }
    countFile.close();
  }

  // 2. Zapíšeme nový řádek
  isWriting = true;
  File file = LittleFS.open(filename, FILE_APPEND);
  if (file) {
    file.print("\"" + getTimestamp() + "\"");
    for(int i=0; i<7; i++) {
      file.print(","); file.print(t[i], 1);
    } 
    file.print(","); file.println(spal, 1);
    file.close();
    lineCount++; // Přičteme právě zapsaný řádek
  }
  isWriting = false;

  // 3. Pokud jsme přesáhli limit, provedeme rotaci
  if (lineCount > maxRecords) rotateFiles();
}

// Získání doby provozu ve formátu "X dny, Y hodin, Z minut"
String getUptime() {
  unsigned long ms = millis(); // Získání doby provozu v milisekundách
  long days = ms / (24L * 3600L * 1000L); // Počet dní
  long hours = (ms % (24L * 3600L * 1000L)) / (3600L * 1000L); // Počet hodin
  long minutes = (ms % (3600L * 1000L)) / (60L * 1000L); // Počet minut
  
  String res = ""; 
  if (days > 0) { res += String(days) + " dny, "; } // Přidání dnů, pokud jsou
  res += String(hours) + " hodin, "; // Přidání hodin
  res += String(minutes) + " minut"; // Přidání minut
  return res; // Vrácení formátovaného řetězce
}

// --- WEBOVÉ STRÁNKY ---
void handleRoot() {
  // --- DASHBOARD STRÁNKA ---
  if (isWriting) { 
    server.send(503, "text/plain", "Zapisuji data, zkuste to za vterinu.");
    return;
  }

  // Načtení aktuálních teplot pro první vykreslení
  sensors.requestTemperatures();
  float t[7];
  for(int i=0; i<7; i++) {
    float rawTemp = sensors.getTempC(mojeCidla[i].adr);
    if (rawTemp < -100) t[i] = rawTemp; 
    else t[i] = rawTemp + mojeCidla[i].offset;
  }
  float spal = thermocouple.readCelsius() - spalinyOffset;

  // Vytvoření HTML stránky
  server.sendHeader("Cache-Control", "no-cache");
  server.sendContent("<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>");
  server.sendContent("<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>");
  server.sendContent("<style>body{font-family:sans-serif; text-align:center; background:#f0f2f5; padding:10px;} "
                     ".box{background:white; padding:15px; margin:15px auto; border-radius:10px; max-width:900px; box-shadow:0 2px 4px rgba(0,0,0,0.1);} "
                     ".chart-container{position:relative; height:350px; width:100%;} "
                     ".progress{width:200px; background:#ddd; height:10px; border-radius:5px; margin:5px auto;} "
                     ".bar{height:100%; border-radius:5px;} "
                     "button{padding:6px 12px; margin:2px; cursor:pointer; background:#fff; border:1px solid #ccc; border-radius:4px;} "
                     "button:hover{background:#eee;}</style></head><body>");

  server.sendContent("<h2>Kotelna - Dashboard</h2>");

  // --- UKAZATEL PAMĚTI A DOBY PROVOZU ---
  size_t total = LittleFS.totalBytes();
  size_t used = LittleFS.usedBytes();
  float per = (float)used / total * 100.0;
  server.sendContent("<div style='font-size: 0.8em; color: #666;'>Využití paměti: <b id='memPerc'>" + String(per, 1) + "%</b></div>");
  server.sendContent("<div class='progress'><div class='bar' id='memBar' style='width:"+String(per)+"%; background:"+(per>80?"red":"#28a745")+";'></div></div>");
  server.sendContent("<div style='font-size: 0.9em; color: #666; margin-top:10px;'>Systém běží: <b id='uptimeDisp'>" + getUptime() + "</b></div>");

  // --- OVLÁDACÍ PRVKY PRO ROZSAH ---
  server.sendContent(
    "<div class='box' style='padding:10px;'>"
    "<b>Rozsah grafů:</b> "
    "<button onclick='setRange(36, this)'>6h</button> "
    "<button onclick='setRange(72, this)'>12h</button> "
    "<button onclick='setRange(108, this)'>18h</button> "
    "<button onclick='setRange(144, this)'>24h</button> "
    "<button onclick='setRange(180, this)'>30h</button> "
    "<button onclick='setRange(216, this)'>36h</button> "
    "<button onclick='setRange(252, this)'>42h</button> "
    "<button id='btnDefault' style='background:#ddd' onclick='setRange(288, this)'>Vše (48h)</button>"
    "</div>"
  );

  // SEKCÍ 1: KOTEL A SPALINY
  server.sendContent("<div class='box'><h3>1. Teplota Kotle a Spalin</h3>"
                     "<div style='display:flex; justify-content:center; flex-wrap:wrap; margin-bottom:15px;'>"
                     "<div>Vstup: <b id='temp0'>"+String(t[0],1)+"°C</b></div><div style='margin:0 15px;'>Výstup: <b id='temp1'>"+String(t[1],1)+"°C</b></div>"
                     "<div style='color:red;'>Spaliny: <b id='tempSpal'>"+String(spal,0)+"°C</b></div></div>"
                     "<div class='chart-container'><canvas id='chartKotel'></canvas></div></div>");

  // SEKCE 2: AKUMULACE
  server.sendContent("<div class='box'><h3>2. Akumulační nádrže</h3>"
                     "<div style='display:flex; justify-content:center; flex-wrap:wrap; gap:15px; margin-bottom:15px;'>"
                       "<div>Horní: <b id='temp2'>"+String(t[2],1)+"°C</b></div>"
                       "<div>Střed 1: <b id='temp3'>"+String(t[3],1)+"°C</b></div>"
                       "<div>Střed 2: <b id='temp4'>"+String(t[4],1)+"°C</b></div>"
                       "<div>Spodek: <b id='temp5'>"+String(t[5],1)+"°C</b></div>"
                     "</div>"
                     "<div class='chart-container'><canvas id='chartAku'></canvas></div></div>");

  // SEKCE 3: VENKOVNÍ TEPLOTA
  server.sendContent("<div class='box'><h3>3. Venkovní teplota</h3>"
                     "<div style='margin-bottom:10px;'>Aktuálně venku: <b id='temp6'>"+String(t[6],1)+"°C</b></div>"
                     "<div class='chart-container'><canvas id='chartVenku'></canvas></div></div>");

  // --- JAVASCRIPT ---
  server.sendContent("<script>var rawData = [");
  auto streamFileToJS = [&](const char* path) {
    File f = LittleFS.open(path, FILE_READ);
    if (f) {
      int lineCount = 0;
      while(f.available()) {
        String line = f.readStringUntil('\n'); line.trim();
        if (line.length() > 5 && lineCount++ % 10 == 0) {
          if (line.startsWith("\"")) server.sendContent("[" + line + "],");
          else {
            int c = line.indexOf(',');
            if (c > 0) server.sendContent("[\"" + line.substring(0, c) + "\"" + line.substring(c) + "],");
          }
        }
      }
      f.close();
    }
  };
  streamFileToJS(oldFilename);
  streamFileToJS(filename);

  server.sendContent("]; var currentRange = 288;");
  server.sendContent("var labels = rawData.map(r => r[0]);");

  // Definice funkcí grafů
  server.sendContent(
    "var cKotel, cAku, cVenku;"
    "function drawGraph(id, datasets) {"
    "  return new Chart(document.getElementById(id), {"
    "    type: 'line', data: {labels: labels.slice(-currentRange), datasets: datasets.map(ds => ({...ds, data: ds.data.slice(-currentRange)}))},"
    "    options: { responsive: true, maintainAspectRatio: false, "
    "      scales: { y: { ticks: { stepSize: 5 } }, x: { ticks: { callback: function(val, idx) { let lbl = this.getLabelForValue(val); return idx % 2 === 0 ? lbl : ''; }, maxRotation: 0, autoSkip: true } } },"
    "      plugins: { legend: { position: 'bottom' } }, elements: { point: { radius: 0 }, line: { tension: 0.2 } } }"
    "  });"
    "}"
    "function setRange(p, btn) {"
    "  currentRange = p;"
    "  var btns = btn.parentNode.getElementsByTagName('button');"
    "  for(var i=0; i<btns.length; i++) btns[i].style.background = '#fff';"
    "  btn.style.background = '#ddd';"
    "  refreshCharts();"
    "}"
    "function refreshCharts() {"
    "  let lbls = rawData.map(r => r[0]).slice(-currentRange);"
    "  const up = (c, idxs) => {"
    "    c.data.labels = lbls;"
    "    c.data.datasets.forEach((ds, i) => { ds.data = rawData.map(r => r[idxs[i]]).slice(-currentRange); });"
    "    c.update('none');"
    "  };"
    "  up(cKotel, [1, 2, 8]); up(cAku, [3, 4, 5, 6]); up(cVenku, [7]);"
    "}"
  );

  // Inicializace grafů
  server.sendContent("cKotel = drawGraph('chartKotel', [{label:'Vstup',borderColor:'blue',data:rawData.map(r=>r[1])},{label:'Výstup',borderColor:'orange',data:rawData.map(r=>r[2])},{label:'Spaliny',borderColor:'red',data:rawData.map(r=>r[8])}]);");
  server.sendContent("cAku = drawGraph('chartAku', [{label:'Horní',borderColor:'darkred',data:rawData.map(r=>r[3])},{label:'Střed 1',borderColor:'red',data:rawData.map(r=>r[4])},{label:'Střed 2',borderColor:'orange',data:rawData.map(r=>r[5])},{label:'Dolní',borderColor:'blue',data:rawData.map(r=>r[6])}]);");
  server.sendContent("cVenku = drawGraph('chartVenku', [{label:'Venkovní',borderColor:'green',data:rawData.map(r=>r[7])}]);");

  // AJAX Update
  server.sendContent(
    "async function updateDashboard() {"
    "  try {"
    "    const res = await fetch('/api/data'); const d = await res.json();"
    "    document.getElementById('uptimeDisp').innerText = d.uptime;"
    "    d.t.forEach((val, i) => { document.getElementById('temp'+i).innerText = val.toFixed(1)+'°C'; });"
    "    document.getElementById('tempSpal').innerText = d.spal.toFixed(0)+'°C';"
    "    let now = new Date().toLocaleTimeString([], {hour:'2-digit', minute:'2-digit'});"
    "    rawData.push([now, ...d.t, d.spal]); if(rawData.length > 500) rawData.shift();"
    "  } catch(e) { console.log('Update failed'); }"
    "}"
    "setInterval(updateDashboard, 60000);" 
    "setInterval(refreshCharts, 600000);"
  );
  server.sendContent("</script>");

  // Tlačítka pod čarou
  server.sendContent("<div style='margin: 30px 0;'>"
                     "<a href='/list_page' style='display:inline-block; padding:10px 20px; background:#28a745; color:white; text-decoration:none; border-radius:5px; margin:5px;'>Zobrazit tabulku záznamů</a>"
                     "<a href='/scan' style='display:inline-block; padding:10px 20px; background:#6c757d; color:white; text-decoration:none; border-radius:5px; margin:5px;'>Skenovat čidla</a><br>"
                     "<a href='/restart' onclick='return confirm(\"Opravdu restartovat?\")' style='display:inline-block; padding:8px 15px; background:#007bff; color:white; text-decoration:none; border-radius:5px; margin:20px 5px 5px 5px; font-size: 0.9em;'>Restartovat ESP</a><br><br>"
                     "<a href='/delete' onclick='return confirm(\"Smazat historii?\")' style='color:red; font-size: 0.8em; text-decoration:none; border:1px solid red; padding:5px 10px; border-radius:5px;'>Smazat data a restartovat</a>"
                     "</div></body></html>");
}

// Funkce pro skenování čidel
void handleScan() {
  // Pokud probíhá zápis, vrať 503
  String out = "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  // Stylování stránky
  out += "<style>body{font-family:sans-serif; background:#f0f2f5; text-align:center; padding:10px;}";
  out += "table{width:100%; max-width:900px; margin:20px auto; border-collapse:collapse; background:white; box-shadow:0 2px 4px rgba(0,0,0,0.1);}";
  out += "th, td{padding:10px; border:1px solid #ddd; font-family:monospace; font-size:0.9em;} th{background:#eee;}";
  out += ".new{background:#fff3cd; font-weight:bold; color:#856404;} .known{color:#28a745; font-weight:bold;}";
  out += ".btn{display:inline-block; padding:10px 20px; margin:10px; background:#007bff; color:white; text-decoration:none; border-radius:5px; font-weight:bold;}</style></head><body>";
  // Hlavní nadpis a tlačítka
  out += "<h2>Skenování OneWire sběrnice</h2>";
  out += "<a href='/' class='btn'>← Zpět na Dashboard</a>";
  out += "<a href='/scan' class='btn' style='background:#28a745;'>Aktualizovat sken</a>";
  // Tabulka výsledků
  out += "<table><tr><th>#</th><th>Nalezená adresa (pro kód)</th><th>Stav / Přiřazení</th></tr>";

  // Skenování sběrnice
  byte addr[8]; // Pole pro adresu čidla
  int count = 0; // Počítadlo čidel
  oneWire.reset_search(); // Reset vyhledávání čidel

  // Smyčka pro nalezení všech čidel na sběrnici
  while (oneWire.search(addr)) {
    count++; // Zvýšení počítadla čidel
    
    // Formátování adresy pro snadné kopírování do kódu
    String hexAddr = "{";
    for (uint8_t i = 0; i < 8; i++) {
      hexAddr += "0x";
      if (addr[i] < 16) hexAddr += "0";
      hexAddr += String(addr[i], HEX);
      if (i < 7) hexAddr += ", ";
    }
    hexAddr += "}";

    // Kontrola, zda je čidlo známé
    String name = "NEZNÁMÉ ČIDLO (NOVÉ)";
    bool known = false;

    // Prohledání tvého pole mojeCidla (velikost 7)
    for (int i = 0; i < 7; i++) {
      if (memcmp(addr, mojeCidla[i].adr, 8) == 0) {
        name = mojeCidla[i].name;
        known = true;
        break;
      }
    }
    // Přidání řádku do tabulky
    out += "<tr" + String(known ? "" : " class='new'") + ">";
    out += "<td>" + String(count) + "</td>";
    out += "<td>" + hexAddr + "</td>";
    out += "<td>" + (known ? "<span class='known'>✓ " + name + "</span>" : "⚠️ " + name) + "</td>";
    out += "</tr>";
  }
  // Pokud nebyla nalezena žádná čidla
  if (count == 0) {
    out += "<tr><td colspan='3' style='padding:20px; color:red;'>Nebyla nalezena ŽÁDNÁ čidla! Zkontrolujte napájení a data pin (GPIO 4).</td></tr>";
  }

  // Ukončení tabulky a přidání tipů
  out += "</table><p>Na sběrnici je aktuálně <b>" + String(count) + "</b> aktivních čidel.</p>";
  out += "<div style='background:#fff; padding:15px; max-width:800px; margin:auto; font-size:0.8em; color:#666; text-align:left; border-left:4px solid #007bff;'>";
  out += "<b>Tip:</b> Pokud vidíš žlutý řádek, čidlo je připojené, ale jeho adresa není v kódu. Zkopíruj adresu v závorce a vlož ji do pole <i>mojeCidla</i>.";
  out += "</div><br><a href='/' class='btn'>← Zpět na Dashboard</a></body></html>";
  
  server.send(200, "text/html", out);
}

// Stránka pro zobrazení kompletní historie v tabulce
void handleListPage() {
  if (isWriting) {
    server.send(503, "text/plain", "Zapisuji data...");
    return;
  }

  // Posíláme hlavičku hned, abychom neplnili RAM řetězcem 'out'
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  // Začátek HTML stránky
  server.sendContent("<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>");
  server.sendContent("<style>body{font-family:sans-serif; background:#f0f2f5; text-align:center; padding:10px;}");
  server.sendContent("table{width:100%; max-width:850px; margin:20px auto; border-collapse:collapse; background:white; box-shadow:0 2px 4px rgba(0,0,0,0.1);}");
  server.sendContent("th, td{padding:8px 5px; border:1px solid #ddd; font-size: 0.85em;} th{background:#eee;}");
  server.sendContent(".sep{background:#007bff; color:white; font-weight:bold; padding:10px;}");
  server.sendContent(".btn{display:inline-block; padding:10px 20px; margin:10px; background:#007bff; color:white; text-decoration:none; border-radius:5px;}</style></head><body>");
  // Hlavní nadpis
  server.sendContent("<h2>Kompletní historie</h2>");
  server.sendContent("<a href='/' class='btn'>← Zpět na Dashboard</a>");
  server.sendContent("<table><tr><th>Čas</th><th>S1</th><th>S2</th><th>S3</th><th>S4</th><th>S5</th><th>S6</th><th>Venku</th><th>Spal</th></tr>");
  // Funkce pro vykreslení řádků ze souboru
  auto renderFileRows = [&](const char* path, String label) {
    if (LittleFS.exists(path)) {
      server.sendContent("<tr><td colspan='9' class='sep'>" + label + "</td></tr>");
      File file = LittleFS.open(path, FILE_READ);
      if (file) {
        while(file.available()){
          String line = file.readStringUntil('\n');
          line.trim();
          if (line.length() > 5) {
            String row = "<tr>";
            int pos = 0;
            while (pos < line.length()) {
              int nextComma = line.indexOf(',', pos);
              String val = (nextComma == -1) ? line.substring(pos) : line.substring(pos, nextComma);
              val.replace("\"", ""); 
              row += "<td>" + val + "</td>";
              if (nextComma == -1) break;
              pos = nextComma + 1;
            }
            row += "</tr>";
            server.sendContent(row); // Posíláme řádek po řádku
          }
        }
        file.close();
      }
    }
  };
  // Vykreslení řádků ze starého a aktuálního souboru
  renderFileRows(oldFilename, "Starší historie (archiv)");
  renderFileRows(filename, "Aktuální záznamy");
  // Ukončení tabulky a přidání tlačítka zpět
  server.sendContent("</table><br><a href='/' class='btn'>← Zpět na Dashboard</a></body></html>");
  server.sendContent(""); // Ukončení přenosu
}

// Inicializace
void setup() {
  Serial.begin(115200); // Inicializace sériové komunikace
  LittleFS.begin(true); // Inicializace LittleFS
  sensors.begin(); // Inicializace Dallas čidel

  // Nastavení rozlišení čidel na 10 bitů
  for(int i=0; i<7; i++) {
    sensors.setResolution(mojeCidla[i].adr, 10);
  }
  
  // Připojení k WiFi síti
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); }
  
  // Nastavení OTA aktualizací
  ArduinoOTA.setHostname("ESP32-Kotelna");
  ArduinoOTA.begin();
  httpUpdater.setup(&server, "/update"); 

  // Nastavení času přes NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  // Nastavení webových stránek
  server.on("/", handleRoot);
  server.on("/scan", handleScan); // Stránka pro skenování čidel
  server.on("/list", []() { // Stránka pro zobrazení obsahu logu
    File file = LittleFS.open(filename, FILE_READ);
    server.streamFile(file, "text/plain");
    file.close();
  });
  server.on("/delete", []() { // Stránka pro smazání historie a restart
    LittleFS.remove(filename); // Smazat aktuální soubor
    LittleFS.remove(oldFilename); // Smazat i archiv!
    // Vytvoření jednoduché HTML stránky s informací o restartu
    String html = "<html><head><meta charset='UTF-8'>";
    html += "<script>setTimeout(function(){ window.location.href = '/'; }, 8000);</script>"; // Přesměrování za 8s
    html += "<style>body{font-family:sans-serif; text-align:center; padding-top:50px; background:#f0f2f5;}</style>";
    html += "</head><body>";
    html += "<h2>Historie smazána</h2>";
    html += "<p>Systém se restartuje a čistí grafy. Počkejte prosím...</p>";
    html += "</body></html>";
    // Odeslání stránky
    server.send(200, "text/html", html);
    delay(1000);
    ESP.restart(); // Restart ESP
  });
  server.on("/restart", []() { // Stránka pro restart bez mazání
    String html = "<html><head><meta charset='UTF-8'>"; // Základní HTML hlavička
    html += "<script>setTimeout(function(){ window.location.href = '/'; }, 8000);</script>"; // Přesměrování za 8s
    html += "<style>body{font-family:sans-serif; text-align:center; padding-top:50px; background:#f0f2f5;}</style>"; // Stylování stránky
    html += "</head><body>"; // Tělo stránky
    html += "<h2>Systém se restartuje...</h2>"; // Nadpis
    html += "<p>Počkejte prosím, za chvíli vás automaticky přesměruju zpět.</p>"; // Instrukce pro uživatele
    html += "<div style='margin:20px;'>⏳</div>"; // Ikona načítání
    html += "</body></html>"; // Ukončení HTML

    server.send(200, "text/html", html); // Odeslání stránky
    delay(1000); // Krátká pauza před restartem
    ESP.restart(); // Restart ESP
  });
  server.on("/api/data", []() {
    sensors.requestTemperatures();
    float t[7];
    for(int i=0; i<7; i++) {
      float raw = sensors.getTempC(mojeCidla[i].adr);
      t[i] = (raw < -100) ? raw : raw + mojeCidla[i].offset;
    }
    float spal = thermocouple.readCelsius() - spalinyOffset;
    
    String json = "{";
    json += "\"t\":[";
    for(int i=0; i<7; i++) { json += String(t[i], 1) + (i<6?",":""); }
    json += "], \"spal\":" + String(spal, 1);
    json += ", \"uptime\":\"" + getUptime() + "\"";
    json += "}";
    server.send(200, "application/json", json);
  });
  server.on("/list_page", handleListPage); // Stránka s tabulkou záznamů

  server.begin(); // Spuštění webového serveru
}

// Hlavní smyčka
void loop() {
  ArduinoOTA.handle(); // Zpracování OTA aktualizací
  server.handleClient(); // Zpracování webových požadavků
  
  // Logování dat v nastaveném intervalu
  if (millis() - lastLogTime >= logInterval) { // Čas pro logování
    lastLogTime = millis(); // Aktualizace času posledního logování
    sensors.requestTemperatures(); // Požadavek na čtení teplot
    float temps[7]; // Pole pro uložení teplot
    for(int i=0; i<7; i++) {
      float rawTemp = sensors.getTempC(mojeCidla[i].adr);
      // Pokud je čidlo odpojené, neaplikuj korekci, ať vidíme -127
      if (rawTemp < -100) {
        temps[i] = rawTemp; 
      } else {
        temps[i] = rawTemp + mojeCidla[i].offset; // Tady se přičte offset
      }
    }
    
    logData(temps, thermocouple.readCelsius() - spalinyOffset); // Zápis dat do souboru s korekcí spalin
  }
}