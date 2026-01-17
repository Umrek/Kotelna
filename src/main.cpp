#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "max6675.h"
#include "time.h"

// --- Konfigurace WiFi ---
const char* ssid = "zdrahala_Mikrotik";
const char* password = "mojewifi65";

// --- Nastavení času (NTP) ---
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600;      // UTC+1 (Praha)
const int   daylightOffset_sec = 3600; // Letní čas

// --- Nastavení logování ---
const char* filename = "/history.csv";
unsigned long lastLogTime = 0;
const unsigned long logInterval = 300000; // 5 minut (5 * 60 * 1000 ms)
const int maxRecords = 576;               // 48 hodin při 5min intervalu

WebServer server(80);
OneWire oneWire(4);
DallasTemperature sensors(&oneWire);
MAX6675 thermocouple(18, 5, 19);

// Funkce pro získání formátovaného času
String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "00:00";
  char timeStringBuff[10];
  strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M", &timeinfo);
  return String(timeStringBuff);
}

// Funkce pro promazání starých dat (udržuje maxRecords)
void trimFile() {
  File file = LittleFS.open(filename, FILE_READ);
  if (!file) return;

  String lines[maxRecords];
  int count = 0;

  while (file.available()) {
    String line = file.readStringUntil('\n');
    if (line.length() > 0) {
      if (count < maxRecords) {
        lines[count] = line;
        count++;
      } else {
        // Posuneme pole (smažeme nejstarší)
        for (int i = 0; i < maxRecords - 1; i++) lines[i] = lines[i + 1];
        lines[maxRecords - 1] = line;
      }
    }
  }
  file.close();

  // Zapíšeme zpět promazaný soubor
  File outFile = LittleFS.open(filename, FILE_WRITE);
  for (int i = 0; i < count; i++) {
    outFile.println(lines[i]);
  }
  outFile.close();
}

void logData(float v1, float v2, float spal) {
  String timestamp = getTimestamp();
  File file = LittleFS.open(filename, FILE_APPEND);
  if (file) {
    file.printf("%s,%.1f,%.1f,%.1f\n", timestamp.c_str(), v1, v2, spal);
    file.close();
  }
  
  // Kontrola počtu řádků
  int lineCount = 0;
  File countFile = LittleFS.open(filename, FILE_READ);
  while(countFile.available()) { if(countFile.read() == '\n') lineCount++; }
  countFile.close();
  
  if (lineCount > maxRecords) {
    Serial.println("Promazávám historii...");
    trimFile();
  }
}

void handleRoot() {
  sensors.requestTemperatures();
  float v1 = sensors.getTempCByIndex(0);
  float v2 = sensors.getTempCByIndex(1);
  float spal = thermocouple.readCelsius() - 8.0;

  String html = "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>";
  html += "<style>body{font-family:sans-serif; text-align:center; background:#f0f2f5; margin:0; padding:10px;}";
  html += ".box{background:white; padding:15px; margin:10px auto; border-radius:10px; max-width:500px; box-shadow:0 2px 4px rgba(0,0,0,0.1);}</style></head><body>";
  
  html += "<h2>Kotelna - Aktuálně</h2>";
  html += "<div class='box'><b style='color:blue'>Vstup: " + String(v1, 1) + "°C</b> | <b style='color:darkcyan'>Výstup: " + String(v2, 1) + "°C</b></div>";
  html += "<div class='box' style='color:red'><b>Spaliny: " + String(spal, 0) + " °C</b></div>";
  
  html += "<div class='box' style='max-width:800px;'><canvas id='myChart'></canvas></div>";
  
  html += "<script>var ctx=document.getElementById('myChart').getContext('2d');";
  html += "var chart=new Chart(ctx,{type:'line',data:{labels:[";
  
  // Načtení dat pro graf
  File file = LittleFS.open(filename, FILE_READ);
  String l="", d1="", d2="", d3="";
  while(file.available()){
    String line = file.readStringUntil('\n');
    int c1 = line.indexOf(',');
    int c2 = line.indexOf(',', c1+1);
    int c3 = line.indexOf(',', c2+1);
    l += "'" + line.substring(0, c1) + "',";
    d1 += line.substring(c1+1, c2) + ",";
    d2 += line.substring(c2+1, c3) + ",";
    d3 += line.substring(c3+1) + ",";
  }
  file.close();

  html += l + "],datasets:[";
  html += "{label:'Vstup', data:["+d1+"], borderColor:'blue', tension:0.3, pointRadius:0},";
  html += "{label:'Výstup', data:["+d2+"], borderColor:'darkcyan', tension:0.3, pointRadius:0},";
  html += "{label:'Spaliny', data:["+d3+"], borderColor:'red', tension:0.3, pointRadius:0}";
  html += "]}});</script></body></html>";

  server.send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);
  if(!LittleFS.begin(true)) Serial.println("LittleFS Error");
  
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  
  // Inicializace času
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  sensors.begin();
  server.on("/", handleRoot);
  server.begin();
}

void loop() {
  server.handleClient();
  
  if (millis() - lastLogTime >= logInterval) {
    lastLogTime = millis();
    sensors.requestTemperatures();
    logData(sensors.getTempCByIndex(0), sensors.getTempCByIndex(1), thermocouple.readCelsius() - 8.0);
  }
}