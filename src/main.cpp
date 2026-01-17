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
const long  gmtOffset_sec = 3600;      
const int   daylightOffset_sec = 3600; 

// --- Nastavení logování ---
const char* filename = "/history.csv";
unsigned long lastLogTime = 0;
const unsigned long logInterval = 300000; 
const int maxRecords = 576;               

WebServer server(80);
OneWire oneWire(4);
DallasTemperature sensors(&oneWire);
MAX6675 thermocouple(18, 5, 19);

// Pomocná funkce pro vypsání adresy senzoru
void printAddress(DeviceAddress deviceAddress) {
  for (uint8_t i = 0; i < 8; i++) {
    if (deviceAddress[i] < 16) Serial.print("0");
    Serial.print(deviceAddress[i], HEX);
  }
}

String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "00:00";
  char timeStringBuff[10];
  strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M", &timeinfo);
  return String(timeStringBuff);
}

void trimFile() {
  File file = LittleFS.open(filename, FILE_READ);
  if (!file) return;
  String lines[maxRecords];
  int count = 0;
  while (file.available() && count < maxRecords) {
    lines[count++] = file.readStringUntil('\n');
  }
  file.close();

  File outFile = LittleFS.open(filename, FILE_WRITE);
  for (int i = 1; i < count; i++) {
    outFile.println(lines[i]);
  }
  outFile.close();
}

void logData(float t[], float spal) {
  String timestamp = getTimestamp();
  File file = LittleFS.open(filename, FILE_APPEND);
  if (file) {
    file.print(timestamp);
    for(int i=0; i<7; i++) {
      file.print(","); file.print(t[i], 1);
    }
    file.print(","); file.println(spal, 1);
    file.close();
  }
  
  int lineCount = 0;
  File countFile = LittleFS.open(filename, FILE_READ);
  while(countFile.available()) { if(countFile.read() == '\n') lineCount++; }
  countFile.close();
  
  if (lineCount > maxRecords) trimFile();
}

void handleRoot() {
  // Místo pouhého sensors.requestTemperatures(); zkus toto:
  sensors.setWaitForConversion(true); // ESP počká, až senzory doměří
  sensors.requestTemperatures();
  delay(100); // Krátká pauza pro stabilizaci sběrnice
  float t[7];
  Serial.println("\n--- Aktuální čtení ---");
  for(int i=0; i<7; i++) {
    t[i] = sensors.getTempCByIndex(i);
    Serial.print("Senzor "); Serial.print(i); Serial.print(": ");
    Serial.print(t[i]); Serial.println(" °C");
  }
  float spal = thermocouple.readCelsius() - 8.0;
  Serial.print("Spaliny: "); Serial.println(spal);

  String html = "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>";
  html += "<style>body{font-family:sans-serif; text-align:center; background:#f0f2f5; margin:0; padding:10px;}";
  html += ".box{background:white; padding:15px; margin:10px auto; border-radius:10px; max-width:800px; box-shadow:0 2px 4px rgba(0,0,0,0.1);}</style></head><body>";
  
  html += "<h2>Stav Kotelny</h2>";
  html += "<div class='box' style='display:flex; flex-wrap:wrap; justify-content:center;'>";
  for(int i=0; i<7; i++) {
    String color = (t[i] == -127.0) ? "red" : "black";
    html += "<div style='margin:10px; color:"+color+"'>S"+String(i+1)+": <b>"+String(t[i],1)+"°C</b></div>";
  }
  html += "<div style='margin:10px; color:red;'>Spaliny: <b>"+String(spal,0)+"°C</b></div></div>";
  html += "<div class='box'><canvas id='myChart'></canvas></div>";
  
  html += "<script>var ctx=document.getElementById('myChart').getContext('2d');";
  html += "var chart=new Chart(ctx,{type:'line',data:{labels:[";
  
  File file = LittleFS.open(filename, FILE_READ);
  String l="", d[8];
  while(file.available()){
    String line = file.readStringUntil('\n');
    if(line.length() < 5) continue;
    char *cstr = strdup(line.c_str());
    char *token = strtok(cstr, ",");
    if(token) l += "'" + String(token) + "',";
    for(int i=0; i<8; i++) {
      token = strtok(NULL, ",");
      if(token) d[i] += String(token) + ",";
    }
    free(cstr);
  }
  file.close();

  html += l + "],datasets:[";
  String colors[] = {"blue", "cyan", "green", "orange", "purple", "brown", "black"};
  for(int i=0; i<7; i++) {
    html += "{label:'Voda "+String(i+1)+"', data:["+d[i]+"], borderColor:'"+colors[i]+"', tension:0.3, pointRadius:0},";
  }
  html += "{label:'Spaliny', data:["+d[7]+"], borderColor:'red', borderWidth:3, tension:0.3, pointRadius:0}";
  html += "]}});</script></body></html>";

  server.send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);
  delay(1000); // Čas na inicializaci Serialu
  
  Serial.println("\n\n=== START ESP32 SKENER ===");
  if(!LittleFS.begin(true)) Serial.println("LittleFS Error");
  
  sensors.begin();
  int deviceCount = sensors.getDeviceCount();
  Serial.print("Nalezeno senzoru na sbernici: ");
  Serial.println(deviceCount);

  for (int i = 0; i < deviceCount; i++) {
    DeviceAddress tempDeviceAddress;
    if (sensors.getAddress(tempDeviceAddress, i)) {
      Serial.print("Senzor "); Serial.print(i); Serial.print(" adresa: ");
      printAddress(tempDeviceAddress);
      Serial.println();
    }
  }

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi OK");
  
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  server.on("/", handleRoot);
  server.begin();
}

void loop() {
  server.handleClient();
  
  if (millis() - lastLogTime >= logInterval) {
    lastLogTime = millis();
    sensors.requestTemperatures();
    float t[7];
    for(int i=0; i<7; i++) t[i] = sensors.getTempCByIndex(i);
    float spal = thermocouple.readCelsius() - 8.0;
    logData(t, spal);
  }
}
