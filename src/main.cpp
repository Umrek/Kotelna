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
const char* filename = "/history.csv"; // Název souboru pro logování
unsigned long lastLogTime = 0; // Čas posledního logování
const unsigned long logInterval = 60000; // 1 minuta
const int maxRecords = 2880;             // 48 hodin

// --- KOREKCE ČIDEL ---
const float spalinyOffset = 0.0; // Hodnota korekce pro termočlánek (např. -8.0 stupňů)

// --- KONFIGURACE ČIDEL (ADRESA + NÁZEV) ---
struct SensorConfig {
  DeviceAddress adr;
  String name;
};

// Mapování čidel podle tvého zadání
SensorConfig mojeCidla[] = {
  {{ 0x28, 0x40, 0x43, 0x0c, 0x50, 0x25, 0x06, 0x46 }, "Vstup kotle"},   // S1
  {{ 0x28, 0xcc, 0xf7, 0x88, 0x43, 0x25, 0x06, 0xf8 }, "Výstup kotle"},  // S2
  {{ 0x28, 0xda, 0x01, 0xf4, 0x43, 0x25, 0x06, 0x91 }, "Aku - Horní"},   // S3
  {{ 0x28, 0x66, 0x58, 0xfa, 0x42, 0x25, 0x06, 0x33 }, "Aku - Střed 1"}, // S4
  {{ 0x28, 0x76, 0x9f, 0xbc, 0x43, 0x25, 0x06, 0x59 }, "Aku - Střed 2"}, // S5
  {{ 0x28, 0x15, 0x0e, 0xe4, 0x43, 0x25, 0x06, 0x7d }, "Aku - Dolní"},   // S6
  {{ 0x28, 0xbb, 0x8a, 0x10, 0x43, 0x25, 0x06, 0x99 }, "Venkovní"}       // S7
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

// Ořezání souboru
void trimFile() {
  isWriting = true; // Nastavení indikátoru zápisu
  File file = LittleFS.open(filename, FILE_READ); // Otevření souboru pro čtení
  if (!file) { isWriting = false; return; } // Pokud nelze otevřít, ukonči
  File tempFile = LittleFS.open("/temp.csv", FILE_WRITE); // Dočasný soubor pro zápis
  if (!tempFile) { file.close(); isWriting = false; return; } // Pokud nelze otevřít, ukonči
  for(int i=0; i<300; i++) { if(file.available()) file.readStringUntil('\n'); } // Přeskočení prvních 300 řádků
  while(file.available()) { tempFile.println(file.readStringUntil('\n')); } // Zkopírování zbytku do dočasného souboru
  file.close(); tempFile.close(); // Zavření obou souborů
  LittleFS.remove(filename); // Odstranění původního souboru
  LittleFS.rename("/temp.csv", filename); // Přejmenování dočasného souboru na původní název
  isWriting = false;
}

// Logování dat
void logData(float t[], float spal) { // t[] obsahuje 7 teplotních hodnot
  isWriting = true; // Nastavení indikátoru zápisu
  File file = LittleFS.open(filename, FILE_APPEND); // Otevření souboru pro přidávání dat
  if (file) { // Pokud je soubor otevřen
    file.print("\"" + getTimestamp() + "\""); // Zápis časové značky 
    for(int i=0; i<7; i++) { // Zápis teplotních hodnot
      file.print(","); file.print(t[i], 1); // Jedna desetinná místa
    } 
    file.print(","); file.println(spal, 1); // Zápis teploty spalin
    file.close(); // Zavření souboru
  }
  int lineCount = 0; // Počítadlo řádků
  File countFile = LittleFS.open(filename, FILE_READ); // Otevření souboru pro čtení
  while(countFile.available()) { if(countFile.read() == '\n') lineCount++; } // Počítání řádků
  countFile.close(); // Zavření souboru
  isWriting = false; // Reset indikátoru zápisu
  if (lineCount > maxRecords) trimFile(); // Ořezání souboru, pokud je překročen limit
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
    server.send(503, "text/plain", "Zapisuji data, zkuste to za vterinu."); // Pokud probíhá zápis, vrať 503
    return;
  }

  // Načtení aktuálních teplot
  sensors.requestTemperatures(); // Požadavek na čtení teplot
  float t[7]; // Pole pro uložení teplot
  for(int i=0; i<7; i++) { // Načtení teplot z čidel
    t[i] = sensors.getTempC(mojeCidla[i].adr); // Načtení teploty z čidla
  }
  float spal = thermocouple.readCelsius() - spalinyOffset; // Korekce teploty spalin

  // Vytvoření HTML stránky
  server.sendHeader("Cache-Control", "no-cache"); // Zabránit cachování
  server.sendContent("<html><head><meta charset='UTF-8'><meta http-equiv='refresh' content='60'><meta name='viewport' content='width=device-width, initial-scale=1'>"); // Základní HTML hlavička
  server.sendContent("<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>"); // Načtení Chart.js knihovny
  server.sendContent("<style>body{font-family:sans-serif; text-align:center; background:#f0f2f5; padding:10px;} " // Stylování stránky
                     ".box{background:white; padding:15px; margin:15px auto; border-radius:10px; max-width:900px; box-shadow:0 2px 4px rgba(0,0,0,0.1);} " // Box styl
                     "h3{color:#333; border-bottom:1px solid #eee; padding-bottom:10px;} " // Nadpisy
                     ".chart-container{position:relative; height:350px; width:100%;}</style></head><body>"); // Stylování grafů
  
  server.sendContent("<h2>Kotelna - Dashboard</h2>"); // Hlavní nadpis

  // Zobrazení doby provozu
  server.sendContent("<div style='font-size: 0.9em; color: #666; margin-bottom: 20px;'>Systém běží: <b>" + getUptime() + "</b></div>");

  // SEKCÍ 1: KOTEL A SPALINY
  server.sendContent("<div class='box'><h3>1. Teplota Kotle a Spalin</h3>"
                     "<div style='display:flex; justify-content:center; flex-wrap:wrap; margin-bottom:15px;'>"
                     "<div>Vstup: <b>"+String(t[0],1)+"°C</b></div><div style='margin:0 15px;'>Výstup: <b>"+String(t[1],1)+"°C</b></div>"
                     "<div style='color:red;'>Spaliny: <b>"+String(spal,0)+"°C</b></div></div>"
                     "<div class='chart-container'><canvas id='chartKotel'></canvas></div></div>");

  // SEKCE 2: AKUMULACE
  server.sendContent("<div class='box'><h3>2. Akumulační nádrže</h3>"
                     // Tento kontejner (flex) zajistí zobrazení v jednom řádku
                     "<div style='display:flex; justify-content:center; flex-wrap:wrap; gap:15px; margin-bottom:15px;'>"
                       "<div>Horní: <b>"+String(t[2],1)+"°C</b></div>"
                       "<div>Střed 1: <b>"+String(t[3],1)+"°C</b></div>"
                       "<div>Střed 2: <b>"+String(t[4],1)+"°C</b></div>"
                       "<div>Spodek: <b>"+String(t[5],1)+"°C</b></div>"
                     "</div>"
                     "<div class='chart-container'><canvas id='chartAku'></canvas></div></div>");

  // SEKCE 3: VENKOVNÍ TEPLOTA
  server.sendContent("<div class='box'><h3>3. Venkovní teplota</h3>"
                     "<div style='margin-bottom:10px;'>Aktuálně venku: <b>"+String(t[6],1)+"°C</b></div>"
                     "<div class='chart-container'><canvas id='chartVenku'></canvas></div></div>");

  // --- JAVASCRIPT ---
  server.sendContent("<script>var rawData = ["); // Načtení dat ze souboru
  File file = LittleFS.open(filename, FILE_READ); // Otevření souboru pro čtení
  if (file) { // Pokud je soubor otevřen
    int lineCounter = 0; // Počítadlo řádků
    while(file.available()){ // Čtení řádků
      String line = file.readStringUntil('\n'); line.trim(); // Ořezání bílých znaků
      if (lineCounter++ % 10 == 0 && line.length() > 5) { // Každý 10. řádek pro zmenšení dat
        if (line.startsWith("\"")) { server.sendContent("[" + line + "],"); } // Pokud řádek začíná uvozovkami
        else {
           int firstComma = line.indexOf(','); // Najdi první čárku
           if (firstComma > 0) server.sendContent("[\"" + line.substring(0, firstComma) + "\"" + line.substring(firstComma) + "],"); // Přidej uvozovky kolem časové značky
        }
      }
    }
    file.close(); // Zavření souboru
  }
  server.sendContent("]; var labels = rawData.map(r => r[0]);"); // Extrahování časových značek

  // Funkce pro zjednodušení tvorby grafů
  server.sendContent( // JavaScript funkce pro vykreslení grafu
    "function drawGraph(id, datasets) {" // Funkce pro vykreslení grafu
    "  new Chart(document.getElementById(id), {" // Vytvoření nového grafu
    "    type: 'line', data: {labels: labels, datasets: datasets}," // Data grafu
    "    options: { responsive: true, maintainAspectRatio: false, " // Možnosti grafu
    "    scales: { y: { ticks: { stepSize: 5 } } }, " // Nastavení osy Y
    "    plugins: { legend: { position: 'bottom' } }, elements: { point: { radius: 0 } } }" // Další možnosti grafu
    "  });"
    "}"
  );

  // GRAF 1: KOTEL (S1, S2) + SPALINY (r[8])
  server.sendContent(
    "drawGraph('chartKotel', ["
    "  {label:'Vstup kotle', borderColor:'blue', data: rawData.map(r => r[1])},"
    "  {label:'Výstup kotle', borderColor:'orange', data: rawData.map(r => r[2])},"
    "  {label:'Spaliny', borderColor:'red', data: rawData.map(r => r[8])}"
    "]);"
  );

  // GRAF 2: AKUMULACE (S3, S4, S5, S6)
  server.sendContent(
    "drawGraph('chartAku', ["
    "  {label:'Aku - Horní', borderColor:'darkred', data: rawData.map(r => r[3])},"
    "  {label:'Aku - Střed 1', borderColor:'red', data: rawData.map(r => r[4])},"
    "  {label:'Aku - Střed 2', borderColor:'orange', data: rawData.map(r => r[5])},"
    "  {label:'Aku - Dolní', borderColor:'blue', data: rawData.map(r => r[6])}"
    "]);"
  );

  // GRAF 3: VENKOV (S7)
  server.sendContent(
    "drawGraph('chartVenku', ["
    "  {label:'Venkovní teplota', borderColor:'green', data: rawData.map(r => r[7])}"
    "]);"
  );
  server.sendContent("</script>");

  // SEKCE 4: Tlačítka pod grafy
  server.sendContent("<div style='margin: 30px 0;'>");
  // Tlačítko pro tabulku (zelené)
  server.sendContent("<a href='/list_page' style='display:inline-block; padding:10px 20px; background:#28a745; color:white; text-decoration:none; border-radius:5px; margin:5px;'>Zobrazit tabulku záznamů</a>");
  // Tlačítko pro skenování čidel (šedé)
  server.sendContent("<a href='/scan' style='display:inline-block; padding:10px 20px; background:#6c757d; color:white; text-decoration:none; border-radius:5px; margin:5px;'>Skenovat čidla</a><br>");
  // Tlačítko pro čistý restart (modré)
  server.sendContent("<a href='/restart' onclick='return confirm(\"Opravdu restartovat ESP bez mazani dat?\")' style='display:inline-block; padding:8px 15px; background:#007bff; color:white; text-decoration:none; border-radius:5px; margin:20px 5px 5px 5px; font-size: 0.9em;'>Restartovat ESP</a>");
  server.sendContent("<br><br>");
  // Tlačítko pro smazání (červené, menší)
  server.sendContent("<a href='/delete' onclick='return confirm(\"POZOR: Opravdu smazat celou historii?\")' style='color:red; font-size: 0.8em; text-decoration:none; border:1px solid red; padding:5px 10px; border-radius:5px;'>Smazat data a restartovat</a>");
  server.sendContent("</div>");

  server.sendContent("</body></html>");
}

// Funkce pro skenování čidel
void handleScan() {
  String out = "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  out += "<style>body{font-family:sans-serif; background:#f0f2f5; text-align:center; padding:10px;}";
  out += "table{width:100%; max-width:800px; margin:20px auto; border-collapse:collapse; background:white;}";
  out += "th, td{padding:10px; border:1px solid #ddd; font-family:monospace;} th{background:#eee;}";
  out += ".new{background:#ffcccc; font-weight:bold;} .known{color:#28a745;}";
  out += ".btn{display:inline-block; padding:10px 20px; margin:10px; background:#007bff; color:white; text-decoration:none; border-radius:5px;}</style></head><body>";
  
  out += "<h2>Skenování čidel na sběrnici</h2>";
  out += "<a href='/' class='btn'>← Zpět na Dashboard</a>";
  out += "<a href='/scan' class='btn' style='background:#28a745;'>Znovu oskenovat</a>";
  
  out += "<table><tr><th>Pořadí</th><th>Adresa (HEX)</th><th>Stav / Název</th></tr>";

  byte addr[8];
  int count = 0;
  oneWire.reset_search();

  while (oneWire.search(addr)) {
    count++;
    String hexAddr = "";
    for (uint8_t i = 0; i < 8; i++) {
      if (addr[i] < 16) hexAddr += "0";
      hexAddr += String(addr[i], HEX);
      if (i < 7) hexAddr += ", ";
    }

    // Porovnání s tvými známými čidly
    String name = "NEZNÁMÉ - NOVÉ ČIDLO!";
    bool known = false;

    // Pole tvých známých adres pro kontrolu
    byte znamaCidla[7][8] = {
      { 0x28, 0x40, 0x43, 0x0c, 0x50, 0x25, 0x06, 0x46 },
      { 0x28, 0xcc, 0xf7, 0x88, 0x43, 0x25, 0x06, 0xf8 },
      { 0x28, 0xda, 0x01, 0xf4, 0x43, 0x25, 0x06, 0x91 },
      { 0x28, 0x66, 0x58, 0xfa, 0x42, 0x25, 0x06, 0x33 },
      { 0x28, 0x76, 0x9f, 0xbc, 0x43, 0x25, 0x06, 0x59 },
      { 0x28, 0x15, 0x0e, 0xe4, 0x43, 0x25, 0x06, 0x7d },
      { 0x28, 0xbb, 0x8a, 0x10, 0x43, 0x25, 0x06, 0x99 }
    };
    String jmenaCidel[] = {"Vstup kotle", "Výstup kotle", "Aku - Horní", "Aku - Střed 1", "Aku - Střed 2", "Aku - Dolní", "Venkovní"};

    for (int i = 0; i < 7; i++) {
      if (memcmp(addr, znamaCidla[i], 8) == 0) {
        name = jmenaCidel[i];
        known = true;
        break;
      }
    }

    out += "<tr" + String(known ? "" : " class='new'") + ">";
    out += "<td>" + String(count) + "</td>";
    out += "<td>0x" + hexAddr + "</td>";
    out += "<td" + String(known ? " class='known'" : "") + ">" + name + "</td>";
    out += "</tr>";
  }

  if (count == 0) {
    out += "<tr><td colspan='3'>Žádná čidla nebyla nalezena! Zkontrolujte zapojení.</td></tr>";
  }

  out += "</table><p>Celkem nalezeno čidel: " + String(count) + "</p>";
  out += "<a href='/' class='btn'>← Zpět na Dashboard</a></body></html>";
  
  server.send(200, "text/html", out);
}

// Funkce pro zobrazení stránky s tabulkou záznamů
void handleListPage() {
  // Pokud probíhá zápis, vrať 503
  if (isWriting) {
    server.send(503, "text/plain", "Zapisuji data, zkuste to za vterinu.");
    return;
  }

  // Vytvoření HTML stránky s tabulkou
  String out = "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  out += "<style>body{font-family:sans-serif; background:#f0f2f5; text-align:center; padding:10px;}";
  out += "table{width:100%; max-width:800px; margin:20px auto; border-collapse:collapse; background:white; box-shadow:0 2px 4px rgba(0,0,0,0.1);}";
  out += "th, td{padding:10px; border:1px solid #ddd; font-size: 0.9em;} th{background:#eee;}";
  out += ".btn{display:inline-block; padding:10px 20px; margin:10px; background:#007bff; color:white; text-decoration:none; border-radius:5px;}</style></head><body>";
  
  out += "<h2>Historie záznamů</h2>";
  out += "<a href='/' class='btn'>← Zpět na Dashboard</a>";
  out += "<table><tr><th>Čas</th><th>S1</th><th>S2</th><th>S3</th><th>S4</th><th>S5</th><th>S6</th><th>Venku</th><th>Spal</th></tr>";

  // Načtení dat ze souboru a vytvoření řádků tabulky
  File file = LittleFS.open(filename, FILE_READ);
  if (file) {
    while(file.available()){
      String line = file.readStringUntil('\n');
      line.trim();
      if (line.length() > 5) {
        out += "<tr>";
        // Rozdělení CSV řádku na buňky
        int pos = 0;
        while (pos < line.length()) {
          int nextComma = line.indexOf(',', pos);
          String val = (nextComma == -1) ? line.substring(pos) : line.substring(pos, nextComma);
          val.replace("\"", ""); // Odstranění uvozovek u času
          out += "<td>" + val + "</td>";
          if (nextComma == -1) break;
          pos = nextComma + 1;
        }
        out += "</tr>";
      }
    }
    file.close();
  }
  
  // Ukončení tabulky a přidání tlačítka zpět
  out += "</table><br><a href='/' class='btn'>← Zpět na Dashboard</a></body></html>";
  server.send(200, "text/html", out);
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
    LittleFS.remove(filename);
    String html = "<html><head><meta charset='UTF-8'>";
    html += "<script>setTimeout(function(){ window.location.href = '/'; }, 8000);</script>"; // Přesměrování za 8s
    html += "<style>body{font-family:sans-serif; text-align:center; padding-top:50px; background:#f0f2f5;}</style>";
    html += "</head><body>";
    html += "<h2>Historie smazána</h2>";
    html += "<p>Systém se restartuje a čistí grafy. Počkejte prosím...</p>";
    html += "</body></html>";
    
    server.send(200, "text/html", html);
    delay(1000);
    ESP.restart();
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
    for(int i=0; i<7; i++) { // Načtení teplot z čidel
      temps[i] = sensors.getTempC(mojeCidla[i].adr); // Načtení teploty z čidla
    }
    logData(temps, thermocouple.readCelsius() - spalinyOffset); // Zápis dat do souboru s korekcí spalin
  }
}