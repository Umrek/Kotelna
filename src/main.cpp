#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h> // Knihovna pro nahrávání firmwaru přes webový prohlížeč (/update)
#include <LittleFS.h>         // Souborový systém pro ukládání historie do flash paměti
#include <OneWire.h>          // Protokol pro komunikaci s digitálními teploměry
#include <DallasTemperature.h> // Knihovna pro ovládání čidel DS18B20
#include <ArduinoOTA.h>       // Podpora pro bezdrátové nahrávání z VS Code/PlatformIO
#include "max6675.h"          // Knihovna pro termočlánek (spaliny)
#include "time.h"             // Práce s reálným časem z internetu

// --- KONFIGURACE SÍTĚ ---
const char* ssid = "zdrahala_Mikrotik";
const char* password = "mojewifi65";

// --- NASTAVENÍ ČASU (NTP) ---
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600;      // Posun času (UTC+1 pro ČR)
const int   daylightOffset_sec = 3600; // Letní čas (+1h)

// --- NASTAVENÍ LOGOVÁNÍ ---
const char* filename = "/history.csv";   // Název souboru s historií
unsigned long lastLogTime = 0;           // Pomocná proměnná pro hlídání intervalu
const unsigned long logInterval = 60000; // Interval zápisu: 1 minuta (60 000 ms)
const int maxRecords = 2880;             // Limit historie: 48 hodin (2 dni po 1440 minutách)

// --- INICIALIZACE OBJEKTŮ ---
WebServer server(80);                    // Webový server na portu 80
HTTPUpdateServer httpUpdater;            // Objekt pro webový update firmwaru
OneWire oneWire(4);                      // Čidla DS18B20 připojena na pin GPIO 4
DallasTemperature sensors(&oneWire);     // Správce teplotních čidel
MAX6675 thermocouple(18, 5, 19);         // Termočlánek: SCK=18, CS=5, SO=19

// Funkce pro získání aktuálního času ve formátu HH:MM
String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "00:00"; // Pokud se čas nepodaří načíst, vrátí nuly
  char timeStringBuff[10];
  strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M", &timeinfo);
  return String(timeStringBuff);
}

// Funkce pro oříznutí souboru, aby nedošlo k zaplnění paměti
void trimFile() {
  File file = LittleFS.open(filename, FILE_READ);
  if (!file) return;

  File tempFile = LittleFS.open("/temp.csv", FILE_WRITE);
  if (!tempFile) { file.close(); return; }

  // Při oříznutí smažeme prvních 300 nejstarších záznamů
  for(int i=0; i<300; i++) {
    if(file.available()) file.readStringUntil('\n');
  }
  
  // Zbytek dat překopírujeme do dočasného souboru (řádek po řádku, šetří RAM)
  while(file.available()) {
    tempFile.println(file.readStringUntil('\n'));
  }
  
  file.close();
  tempFile.close();
  LittleFS.remove(filename);        // Smažeme starý velký soubor
  LittleFS.rename("/temp.csv", filename); // Přejmenujeme dočasný na hlavní
  Serial.println("Historie byla oříznuta o staré záznamy.");
}

// Funkce pro zápis aktuálních teplot na "disk" (Flash)
void logData(float t[], float spal) {
  File file = LittleFS.open(filename, FILE_APPEND);
  if (file) {
    file.print(getTimestamp()); // Zápis času
    for(int i=0; i<7; i++) {    // Zápis 7 čidel vody
      file.print(","); file.print(t[i], 1);
    }
    file.print(","); file.println(spal, 1); // Zápis teploty spalin
    file.close();
  }
  
  // Kontrola počtu řádků (aby soubor nerostl do nekonečna)
  int lineCount = 0;
  File countFile = LittleFS.open(filename, FILE_READ);
  while(countFile.available()) { if(countFile.read() == '\n') lineCount++; }
  countFile.close();
  
  if (lineCount > maxRecords) trimFile(); // Pokud je řádků moc, ořízneme je
}

// Hlavní funkce pro obsluhu webové stránky
void handleRoot() {
  sensors.requestTemperatures(); // Příkaz čidlům k měření
  float t[7];
  for(int i=0; i<7; i++) t[i] = sensors.getTempCByIndex(i);
  float spal = thermocouple.readCelsius() - 8.0; // Korekce termočlánku

  // Odesílání HTML hlavičky - používáme sendContent, abychom nehltili RAM
  server.sendHeader("Cache-Control", "no-cache");
  // Meta refresh=60 zajistí automatické znovunačtení stránky každou minutu
  server.sendContent("<html><head><meta charset='UTF-8'><meta http-equiv='refresh' content='60'><meta name='viewport' content='width=device-width, initial-scale=1'>");
  server.sendContent("<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>"); // Načtení knihovny pro grafy
  server.sendContent("<style>body{font-family:sans-serif; text-align:center; background:#f0f2f5; padding:10px;} .box{background:white; padding:15px; margin:10px auto; border-radius:10px; max-width:800px; box-shadow:0 2px 4px rgba(0,0,0,0.1);}</style></head><body>");
  server.sendContent("<h2>Kotelna - Vzdálená správa</h2><div class='box' style='display:flex; flex-wrap:wrap; justify-content:center;'>");
  
  // Výpis aktuálních teplot do krabiček (S1 - S7)
  for(int i=0; i<7; i++) {
    String color = (t[i] <= -100.0) ? "red" : "black"; // Červená barva, pokud je čidlo odpojené
    server.sendContent("<div style='margin:10px; color:"+color+"'>S"+String(i+1)+": <b>"+String(t[i],1)+"°C</b></div>");
  }
  server.sendContent("<div style='margin:10px; color:red;'>Spaliny: <b>"+String(spal,0)+"°C</b></div></div>");
  server.sendContent("<div class='box'><canvas id='myChart'></canvas></div><script>");
  
  // Inicializace grafu Chart.js
  server.sendContent("var ctx=document.getElementById('myChart').getContext('2d'); var chart=new Chart(ctx,{type:'line',data:{labels:[");

  // Načítání historie ze souboru a sypání dat přímo do Javascriptu v prohlížeči
  File file = LittleFS.open(filename, FILE_READ);
  String d[8];
  while(file.available()){
    String line = file.readStringUntil('\n');
    if(line.length() < 10) continue; // Přeskočí prázdné nebo poškozené řádky
    char *cstr = strdup(line.c_str());
    char *token = strtok(cstr, ",");
    if(token) server.sendContent("'" + String(token) + "',"); // Časové popisky na ose X
    for(int i=0; i<8; i++) {
      token = strtok(NULL, ",");
      if(token) d[i] += String(token) + ","; // Data pro jednotlivé křivky
    }
    free(cstr);
  }
  file.close();

  // Definice barev a názvů křivek v grafu
  server.sendContent("],datasets:[");
  String colors[] = {"blue", "cyan", "green", "orange", "purple", "brown", "black"};
  for(int i=0; i<7; i++) {
    server.sendContent("{label:'Voda "+String(i+1)+"', data:["+d[i]+"], borderColor:'"+colors[i]+"', tension:0.3, pointRadius:0},");
  }
  server.sendContent("{label:'Spaliny', data:["+d[7]+"], borderColor:'red', borderWidth:3, tension:0.3, pointRadius:0}");
  server.sendContent("]}});</script></body></html>");
}

void setup() {
  Serial.begin(115200); // Sériová linka pro ladění
  LittleFS.begin(true); // Spuštění souborového systému (true = zformátovat při chybě)
  sensors.begin();      // Start teplotních čidel
  
  WiFi.begin(ssid, password); // Připojení k WiFi
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  
  // Nastavení názvu zařízení pro síť a spuštění OTA (vzdálený nahrávání kódu)
  ArduinoOTA.setHostname("ESP32-Kotelna");
  ArduinoOTA.begin();

  // Aktivace záložního nahrávání přes web: http://192.168.2.9/update
  httpUpdater.setup(&server, "/update"); 
  
  // Synchronizace času přes internet
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  // Definice cest webového serveru
  server.on("/", handleRoot);
  server.begin();
  Serial.println("\nSystém připraven na 192.168.2.9");
}

void loop() {
  ArduinoOTA.handle();   // Kontrola, zda někdo nechce nahrát nový kód z VS Code
  server.handleClient(); // Vyřizování požadavků na webovou stránku
  
  // Pravidelné logování dat na základě logInterval (1 minuta)
  if (millis() - lastLogTime >= logInterval) {
    lastLogTime = millis();
    sensors.requestTemperatures();
    float temps[7];
    for(int i=0; i<7; i++) temps[i] = sensors.getTempCByIndex(i);
    logData(temps, thermocouple.readCelsius() - 8.0);
  }
}