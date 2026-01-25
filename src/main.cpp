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
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600;      // UTC+1 (střední Evropa)
const int   daylightOffset_sec = 3600; // Letní čas +1h

// --- NASTAVENÍ LOGOVÁNÍ ---
const char* filename = "/history.csv";
unsigned long lastLogTime = 0;
const unsigned long logInterval = 60000; // 1 minuta
const int maxRecords = 2880;             // 48 hodin

// --- KONFIGURACE ČIDEL (ADRESA + NÁZEV) ---
struct SensorConfig {
  DeviceAddress adr;
  String name;
};

// Zde přepiš názvy "S1", "S2" atd. na reálné názvy (např. "Bojler"), až je určíš
SensorConfig mojeCidla[] = {
  {{ 0x28, 0x40, 0x43, 0x0c, 0x50, 0x25, 0x06, 0x46 }, "S1"},
  {{ 0x28, 0xcc, 0xf7, 0x88, 0x43, 0x25, 0x06, 0xf8 }, "S2"},
  {{ 0x28, 0xda, 0x01, 0xf4, 0x43, 0x25, 0x06, 0x91 }, "S3"},
  {{ 0x28, 0x66, 0x58, 0xfa, 0x42, 0x25, 0x06, 0x33 }, "S4"},
  {{ 0x28, 0x76, 0x9f, 0xbc, 0x43, 0x25, 0x06, 0x59 }, "S5"},
  {{ 0x28, 0x15, 0x0e, 0xe4, 0x43, 0x25, 0x06, 0x7d }, "S6"},
  {{ 0x28, 0xbb, 0x8a, 0x10, 0x43, 0x25, 0x06, 0x99 }, "S7 - Venek"}
};

// --- GLOBÁLNÍ PROMĚNNÉ A OBJEKTY ---
bool isWriting = false; // Indikátor probíhajícího zápisu do souboru
WebServer server(80); // Webový server na portu 80
HTTPUpdateServer httpUpdater; // HTTP Updater pro OTA aktualizace
OneWire oneWire(4); // 1-Wire na GPIO4
DallasTemperature sensors(&oneWire); // Dallas teplotní čidla
MAX6675 thermocouple(18, 5, 19); // MAX6675 na SCLK=18, CS=5, MISO=19


String getTimestamp() { // Funkce pro získání aktuálního času ve formátu "HH:MM"
  struct tm timeinfo; // Struktura pro časové informace
  if (!getLocalTime(&timeinfo)) return "00:00"; // Pokud nelze získat čas, vrať "00:00"
  char timeStringBuff[10]; // Buffer pro formátovaný čas
  strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M", &timeinfo); // Formátování času na "HH:MM"
  return String(timeStringBuff); // Vrácení formátovaného času jako String
}

// Funkce pro ořezání CSV souboru na maximální počet záznamů
void trimFile() { // Ořezání souboru na maxRecords záznamů
  isWriting = true; // Nastavení indikátoru zápisu
  File file = LittleFS.open(filename, FILE_READ); // Otevření souboru pro čtení
  if (!file) { isWriting = false; return; } // Pokud nelze otevřít, ukonči
  File tempFile = LittleFS.open("/temp.csv", FILE_WRITE); // Dočasný soubor pro uložení ořezaných dat
  if (!tempFile) { file.close(); isWriting = false; return; } // Pokud nelze otevřít, ukonči
  for(int i=0; i<300; i++) { if(file.available()) file.readStringUntil('\n'); } // Přeskočení starých záznamů
  while(file.available()) { tempFile.println(file.readStringUntil('\n')); } // Zkopírování zbytku do dočasného souboru
  file.close(); tempFile.close(); // Zavření obou souborů
  LittleFS.remove(filename); // Odstranění původního souboru
  LittleFS.rename("/temp.csv", filename); // Přejmenování dočasného souboru na původní název
  isWriting = false; // Reset indikátoru zápisu
}

// Funkce pro logování dat do CSV souboru
void logData(float t[], float spal) { // Logování dat do CSV souboru
  isWriting = true; // Nastavení indikátoru zápisu
  File file = LittleFS.open(filename, FILE_APPEND); // Otevření souboru pro přidávání dat
  if (file) { // Pokud je soubor otevřen
    file.print("\"" + getTimestamp() + "\"");  // Zápis časové značky
    for(int i=0; i<7; i++) { // Zápis teplot z čidel
      file.print(","); file.print(t[i], 1); // Zápis s jedním desetinným místem
    } 
    file.print(","); file.println(spal, 1); // Zápis teploty spalin
    file.close(); // Zavření souboru
  }
  int lineCount = 0; // Počítadlo řádků
  File countFile = LittleFS.open(filename, FILE_READ); // Otevření souboru pro čtení
  while(countFile.available()) { if(countFile.read() == '\n') lineCount++; } // Počítání řádků
  countFile.close(); // Zavření souboru
  isWriting = false; // Reset indikátoru zápisu
  if (lineCount > maxRecords) trimFile(); // Ořezání souboru, pokud je překročen max. počet záznamů
}

// --- WEBOVÉ STRÁNKY ---
void handleRoot() {
  // Pokud probíhá zápis, zobrazíme zprávu a vrátíme se
  if (isWriting) {
    server.send(503, "text/plain", "Zapisuji data, zkuste to za vterinu.");
    return;
  }

  // Načtení aktuálních teplot z čidel
  sensors.requestTemperatures();
  float t[7];
  for(int i=0; i<7; i++) {
    t[i] = sensors.getTempC(mojeCidla[i].adr); // Čtení podle unikátní adresy
  }
  float spal = thermocouple.readCelsius() - 8.0;

  // Vytvoření HTML stránky
  server.sendHeader("Cache-Control", "no-cache");
  server.sendContent("<html><head><meta charset='UTF-8'><meta http-equiv='refresh' content='60'><meta name='viewport' content='width=device-width, initial-scale=1'>");
  server.sendContent("<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>");
  server.sendContent("<style>body{font-family:sans-serif; text-align:center; background:#f0f2f5; padding:10px;} .box{background:white; padding:15px; margin:10px auto; border-radius:10px; max-width:800px; box-shadow:0 2px 4px rgba(0,0,0,0.1);}</style></head><body>");
  server.sendContent("<h2>Kotelna - Dashboard</h2><div class='box' style='display:flex; flex-wrap:wrap; justify-content:center;'>");
  
  // Zobrazení aktuálních teplot
  for(int i=0; i<7; i++) {
    String color = (t[i] <= -100.0) ? "red" : "black";
    server.sendContent("<div style='margin:10px; color:"+color+"'>"+mojeCidla[i].name+": <b>"+String(t[i],1)+"°C</b></div>");
  }
  server.sendContent("<div style='margin:10px; color:red;'>Spaliny: <b>"+String(spal,0)+"°C</b></div></div>");
  server.sendContent("<div class='box'><canvas id='myChart'></canvas></div><script>");
  
  // Načtení dat z CSV souboru pro graf
  server.sendContent("var rawData = [");
  File file = LittleFS.open(filename, FILE_READ);
  if (file) {
    int lineCounter = 0;
    while(file.available()){
      String line = file.readStringUntil('\n');
      line.trim();
      if (lineCounter++ % 10 == 0 && line.length() > 5) {
        if (line.startsWith("\"")) { server.sendContent("[" + line + "],"); }
        else {
           int firstComma = line.indexOf(',');
           if (firstComma > 0) server.sendContent("[\"" + line.substring(0, firstComma) + "\"" + line.substring(firstComma) + "],");
        }
      }
    }
    file.close();
  }
  server.sendContent("];");

  // Dynamické názvy čidel do legendy grafu
  // Vytvoření pole názvů čidel v JavaScriptu
  server.sendContent("var sensorNames = [");
  for(int i=0; i<7; i++) { server.sendContent("'" + mojeCidla[i].name + "',"); }
  // Přidání názvu pro spaliny
  server.sendContent("'Spaliny'];");

  // Vykreslení grafu pomocí Chart.js
  server.sendContent(
    "var labels = rawData.map(r => r[0]);"
    "var colors = ['blue', 'cyan', 'green', 'orange', 'purple', 'brown', 'black', 'red'];"
    "var datasets = [];"
    "for(var i=0; i<8; i++) {"
    "  datasets.push({"
    "    label: sensorNames[i],"
    "    borderColor: colors[i],"
    "    data: rawData.map(r => r[i+1]),"
    "    tension: 0.3, pointRadius: 0"
    "  });"
    "}"
    "var ctx=document.getElementById('myChart').getContext('2d');"
    "ctx.canvas.parentNode.style.height = '450px';" 
    "new Chart(ctx, {"
    "  type: 'line',"
    "  data: {labels: labels, datasets: datasets},"
    "  options: {"
    "    responsive: true, maintainAspectRatio: false,"
    "    scales: {"
    "      y: { ticks: { stepSize: 5 }, beginAtZero: false },"
    "      x: { ticks: { autoSkip: true, maxTicksLimit: 10 } }"
    "    }"
    "  }"
    "}); </script></body></html>"
  );
}

// Funkce pro skenování připojených 1-Wire zařízení
void handleScan() {
  String out = "<h2>Nalezena cidla (ID pro kód):</h2><pre>";
  byte addr[8];
  sensors.begin();
  oneWire.reset_search();
  while (oneWire.search(addr)) {
    out += "DeviceAddress adresa = { ";
    for (int i = 0; i < 8; i++) {
      out += "0x"; if (addr[i] < 16) out += "0";
      out += String(addr[i], HEX); if (i < 7) out += ", ";
    }
    out += " };\n";
  }
  out += "</pre>";
  server.send(200, "text/html", out);
}

// --- SETUP A LOOP ---
void setup() {
  Serial.begin(115200);
  LittleFS.begin(true);
  sensors.begin();

  // Optimalizace rychlosti: Nastavení rozlišení na 10 bitů (0.25°C krok)
  for(int i=0; i<7; i++) {
    sensors.setResolution(mojeCidla[i].adr, 10);
  }
  
  // Připojení k WiFi síti
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); }
  
  // Nastavení OTA a HTTP Updateru
  ArduinoOTA.setHostname("ESP32-Kotelna");
  ArduinoOTA.begin();
  httpUpdater.setup(&server, "/update"); 

  // Nastavení NTP času
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  // Nastavení webového serveru
  server.on("/", handleRoot);
  server.on("/scan", handleScan);
  server.on("/list", []() {
    File file = LittleFS.open(filename, FILE_READ);
    server.streamFile(file, "text/plain");
    file.close();
  });

  server.begin();
}

// Hlavní smyčka
void loop() {
  ArduinoOTA.handle(); // Zpracování OTA aktualizací
  server.handleClient(); // Zpracování HTTP požadavků
  
  // Logování dat každou minutu
  if (millis() - lastLogTime >= logInterval) {
    lastLogTime = millis();
    sensors.requestTemperatures();
    float temps[7];
    for(int i=0; i<7; i++) {
      temps[i] = sensors.getTempC(mojeCidla[i].adr);
    }
    logData(temps, thermocouple.readCelsius() - 8.0);
  }
}