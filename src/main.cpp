#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>
#include <LittleFS.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoOTA.h>
#include "max6675.h"
#include "time.h"

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
const unsigned long logInterval = 60000; // Interval zápisu do paměti (1 minuta)
const int maxRecords = 2880;             // Limit historie (48 hodin při 1min intervalu)

// --- GLOBÁLNÍ PROMĚNNÉ A OBJEKTY ---
bool isWriting = false;                  // Zámek proti kolizi (zápis vs. čtení webem)
WebServer server(80);                    // Webový server na standardním portu 80
HTTPUpdateServer httpUpdater;            // Update firmware přes /update
OneWire oneWire(4);                      // DS18B20 na pinu GPIO 4
DallasTemperature sensors(&oneWire);     // Správce čidel Dallas
MAX6675 thermocouple(18, 5, 19);         // Termočlánek: SCK=18, CS=5, SO=19

// Funkce pro získání času z interních hodin ESP32 (synchronizováno přes NTP)
String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "00:00";
  char timeStringBuff[10];
  strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M", &timeinfo);
  return String(timeStringBuff);
}

// Funkce pro ořezání starých dat (udržuje velikost souboru pod kontrolou)
void trimFile() {
  isWriting = true; 
  Serial.println("Ořezávám historii (limit dosažen)...");
  File file = LittleFS.open(filename, FILE_READ);
  if (!file) { isWriting = false; return; }

  File tempFile = LittleFS.open("/temp.csv", FILE_WRITE);
  if (!tempFile) { file.close(); isWriting = false; return; }

  // Přeskočíme prvních 300 nejstarších řádků
  for(int i=0; i<300; i++) {
    if(file.available()) file.readStringUntil('\n');
  }
  
  // Překopírujeme zbytek do dočasného souboru
  while(file.available()) {
    tempFile.println(file.readStringUntil('\n'));
  }
  
  file.close();
  tempFile.close();
  LittleFS.remove(filename);              // Smazání původního souboru
  LittleFS.rename("/temp.csv", filename); // Přejmenování dočasného na hlavní
  isWriting = false;
  Serial.println("Historie úspěšně oříznuta.");
}

// Funkce pro uložení aktuálních měření na Flash disk (LittleFS)
void logData(float t[], float spal) {
  isWriting = true;
  File file = LittleFS.open(filename, FILE_APPEND);
  if (file) {
    // Čas dáváme do uvozovek pro správnou interpretaci v JavaScriptu (String)
    file.print("\"" + getTimestamp() + "\""); 
    for(int i=0; i<7; i++) {
      file.print(","); file.print(t[i], 1);
    }
    file.print(","); file.println(spal, 1);
    file.close();
  }
  
  // Počítání řádků pro kontrolu limitu
  int lineCount = 0;
  File countFile = LittleFS.open(filename, FILE_READ);
  while(countFile.available()) { if(countFile.read() == '\n') lineCount++; }
  countFile.close();
  
  isWriting = false;
  if (lineCount > maxRecords) trimFile();
}

// Hlavní funkce pro obsluhu webové stránky
void handleRoot() {
  // Pokud ESP32 zrovna ořezává soubor, web počká (prevence pádu)
  if (isWriting) {
    server.send(503, "text/plain", "Zapisuji data (isWriting lock), zkuste to za vterinu.");
    return;
  }

  sensors.requestTemperatures();
  float t[7];
  for(int i=0; i<7; i++) t[i] = sensors.getTempCByIndex(i);
  float spal = thermocouple.readCelsius() - 8.0;

  // Odesílání HTML hlavičky
  server.sendHeader("Cache-Control", "no-cache");
  server.sendContent("<html><head><meta charset='UTF-8'><meta http-equiv='refresh' content='60'><meta name='viewport' content='width=device-width, initial-scale=1'>");
  server.sendContent("<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>");
  server.sendContent("<style>body{font-family:sans-serif; text-align:center; background:#f0f2f5; padding:10px;} .box{background:white; padding:15px; margin:10px auto; border-radius:10px; max-width:800px; box-shadow:0 2px 4px rgba(0,0,0,0.1);}</style></head><body>");
  server.sendContent("<h2>Kotelna - Správa</h2><div class='box' style='display:flex; flex-wrap:wrap; justify-content:center;'>");
  
  // Aktuální teploty v "dlaždicích"
  for(int i=0; i<7; i++) {
    String color = (t[i] <= -100.0) ? "red" : "black";
    server.sendContent("<div style='margin:10px; color:"+color+"'>S"+String(i+1)+": <b>"+String(t[i],1)+"°C</b></div>");
  }
  server.sendContent("<div style='margin:10px; color:red;'>Spaliny: <b>"+String(spal,0)+"°C</b></div></div>");
  
  // Kontejner pro graf
  server.sendContent("<div class='box'><canvas id='myChart'></canvas></div><script>");
  
  // --- RYCHLÝ EXPORT DAT DO JAVASCRIPTU (Jeden průchod souborem) ---
  server.sendContent("var rawData = [");
  File file = LittleFS.open(filename, FILE_READ);
  if (file) {
    int lineCounter = 0;
    while(file.available()){
      String line = file.readStringUntil('\n');
      line.trim();
      // Agregace: Každý 10. řádek (cca každých 10 minut jeden bod v grafu)
      if (lineCounter++ % 10 == 0 && line.length() > 5) {
        // Kontrola a oprava formátu (pokud v souboru chybí uvozovky u času)
        if (line.startsWith("\"")) {
           server.sendContent("[" + line + "],");
        } else {
           int firstComma = line.indexOf(',');
           if (firstComma > 0) {
             server.sendContent("[\"" + line.substring(0, firstComma) + "\"" + line.substring(firstComma) + "],");
           }
        }
      }
    }
    file.close();
  }
  server.sendContent("];");

  // --- LOGIKA GRAFU V PROHLÍŽEČI (Klientský procesor to zvládne lépe než ESP) ---
  server.sendContent(
    "var labels = rawData.map(r => r[0]);"
    "var colors = ['blue', 'cyan', 'green', 'orange', 'purple', 'brown', 'black', 'red'];"
    "var datasets = [];"
    "for(var i=0; i<8; i++) {"
    "  datasets.push({"
    "    label: (i<7 ? 'Voda '+(i+1) : 'Spaliny'),"
    "    borderColor: colors[i],"
    "    data: rawData.map(r => r[i+1]),"
    "    tension: 0.3, pointRadius: 0"
    "  });"
    "}"
    "var ctx=document.getElementById('myChart').getContext('2d');"
    "ctx.canvas.parentNode.style.height = '450px';" // Vynucená výška pro lepší čitelnost na mobilu
    "new Chart(ctx, {"
    "  type: 'line',"
    "  data: {labels: labels, datasets: datasets},"
    "  options: {"
    "    responsive: true,"
    "    maintainAspectRatio: false," // Umožní grafu roztáhnout se do výšky
    "    scales: {"
    "      y: { ticks: { stepSize: 5 }, beginAtZero: false }," // Krok osy Y po 5 stupních
    "      x: { ticks: { maxRotation: 0, autoSkip: true, maxTicksLimit: 10 } }" // Čistá osa X
    "    },"
    "    plugins: { legend: { labels: { boxWidth: 12, font: { size: 10 } } } }"
    "  }"
    "}); </script></body></html>"
  );
}

// Funkce pro obsluhu skenování připojených čidel DS18B20
void handleScan() {
  String out = "<h2>Nalezena cidla:</h2><pre>";
  byte addr[8];
  sensors.begin();
  oneWire.reset_search();
  
  while (oneWire.search(addr)) {
    out += "DeviceAddress adresa = { ";
    for (int i = 0; i < 8; i++) {
      out += "0x";
      if (addr[i] < 16) out += "0";
      out += String(addr[i], HEX);
      if (i < 7) out += ", ";
    }
    out += " };\n";
  }
  out += "</pre><p>Zahrejte jedno cidlo a obnovte stranku (F5), abyste zjistili, ktere to je.</p>";
  server.send(200, "text/html", out);
}
// S1 { 0x28, 0x40, 0x43, 0x0c, 0x50, 0x25, 0x06, 0x46 }
// S2 { 0x28, 0xcc, 0xf7, 0x88, 0x43, 0x25, 0x06, 0xf8 }
// S3 { 0x28, 0xda, 0x01, 0xf4, 0x43, 0x25, 0x06, 0x91 }
// S4 { 0x28, 0x66, 0x58, 0xfa, 0x42, 0x25, 0x06, 0x33 }
// S5 { 0x28, 0x76, 0x9f, 0xbc, 0x43, 0x25, 0x06, 0x59 }
// S6 { 0x28, 0x15, 0x0e, 0xe4, 0x43, 0x25, 0x06, 0x7d }
// S7 { 0x28, 0xbb, 0x8a, 0x10, 0x43, 0x25, 0x06, 0x99 }

void setup() {
  Serial.begin(115200);
  LittleFS.begin(true); // Start souborového systému
  sensors.begin();      // Start čidel DS18B20
  
  // Připojení k WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  
  // Nastavení OTA (aktualizace z VS Code/PlatformIO)
  ArduinoOTA.setHostname("ESP32-Kotelna");
  ArduinoOTA.begin();

  // Web Update (/update)
  httpUpdater.setup(&server, "/update"); 

  // Scan WiFi sítí (/scan)
  server.on("/scan", handleScan);
  
  // Synchronizace času přes internet
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  // Definice cest webserveru
  server.on("/", handleRoot);
  // Pomocná cesta pro kontrolu surových dat
  server.on("/list", []() {
    File file = LittleFS.open(filename, FILE_READ);
    server.streamFile(file, "text/plain");
    file.close();
  });

  server.begin();
  Serial.println("\nHotovo! IP: 192.168.2.9");
}

void loop() {
  ArduinoOTA.handle();   // Obsluha vzdáleného nahrávání kódu
  server.handleClient(); // Obsluha webových požadavků
  
  // Pravidelné měření a logování každou minutu
  if (millis() - lastLogTime >= logInterval) {
    lastLogTime = millis();
    sensors.requestTemperatures();
    float temps[7];
    for(int i=0; i<7; i++) temps[i] = sensors.getTempCByIndex(i);
    logData(temps, thermocouple.readCelsius() - 8.0);
  }
}