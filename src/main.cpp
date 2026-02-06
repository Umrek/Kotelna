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
#include <esp_task_wdt.h> // Knihovna pro watchdog timer

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
int currentLineCount = 0; // Počet aktuálních řádků v souboru (pro kontrolu rotace)
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
    isWriting = true;
    File file = LittleFS.open(filename, FILE_APPEND);
    if (file) {
        file.print("\"" + getTimestamp() + "\"");
        for(int i=0; i<7; i++) {
            file.print(","); file.print(t[i], 1);
        } 
        file.print(","); file.println(spal, 1);
        file.close();
        currentLineCount++; // Jednoduché zvýšení místo počítání celého souboru
    }
    isWriting = false;

    if (currentLineCount >= maxRecords) {
        rotateFiles();
        currentLineCount = 0; // Po rotaci začínáme od nuly
    }
}

// Získání doby provozu ve formátu "X dny, Y hodin, Z minut"
String getUptime() {
    unsigned long ms = millis();
    unsigned long seconds = ms / 1000;
    unsigned long minutes = seconds / 60;
    unsigned long hours = minutes / 60;
    unsigned long days = hours / 24;

    hours %= 24;
    minutes %= 60;
    seconds %= 60;

    char buf[20];
    // Formát: Xd HH:MM:SS
    snprintf(buf, sizeof(buf), "%ldd %02ld:%02ld:%02ld", days, hours, minutes, seconds);
    return String(buf);
}

// --- WEBOVÉ STRÁNKY ---
void handleRoot() {
  if (LittleFS.exists("/index.html")) {
    File file = LittleFS.open("/index.html", "r");
    server.streamFile(file, "text/html");
    file.close();
  } else {
    server.send(404, "text/plain", "Soubor index.html nenalezen! Zapomněl jsi nahrát 'Upload Filesystem Image'?");
  }
}

// Funkce pro skenování čidel
void handleScan() {
  if (LittleFS.exists("/scan.html")) {
    File file = LittleFS.open("/scan.html", "r");
    server.streamFile(file, "text/html");
    file.close();
  } else {
    server.send(404, "text/plain", "Soubor scan.html nenalezen! Zapomněl jsi nahrát 'Upload Filesystem Image'?");
  }
}

// Stránka pro zobrazení kompletní historie v tabulce
void handleListPage() {
  if (LittleFS.exists("/list.html")) {
    File file = LittleFS.open("/list.html", "r");
    server.streamFile(file, "text/html");
    file.close();
  } else {
    server.send(404, "text/plain", "Soubor list.html nenalezen! Zapomněl jsi nahrát 'Upload Filesystem Image'?");
  }
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

  // Zjištění aktuálního počtu řádků v logu (pro kontrolu rotace)
  if (LittleFS.exists(filename)) {
      File f = LittleFS.open(filename, "r");
      while(f.available()) { if(f.read() == '\n') currentLineCount++; }
      f.close();
      Serial.print("Původní počet řádků v logu: "); Serial.println(currentLineCount);
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
  server.on("/scan", handleScan);
  server.on("/list_page", handleListPage); // Stránka s kompletní historií v tabulce

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
  // API endpoint pro získání aktuálních dat ve formátu JSON
  server.on("/api/data", []() {
    sensors.requestTemperatures();
    float t[7];
    for(int i=0; i<7; i++) {
      float raw = sensors.getTempC(mojeCidla[i].adr);
      t[i] = (raw < -100) ? raw : raw + mojeCidla[i].offset;
    }
    float spal = thermocouple.readCelsius() - spalinyOffset;
    
    // Získání síly WiFi signálu
    int rssi = WiFi.RSSI(); 

    String json = "{";
    json += "\"t\":[";
    for(int i=0; i<7; i++) { json += String(t[i], 1) + (i<6?",":""); }
    json += "], \"spal\":" + String(spal, 1);
    json += ", \"uptime\":\"" + getUptime() + "\"";
    json += ", \"rssi\":" + String(rssi); // Přidáno sem
    json += "}";
    server.send(200, "application/json", json);
  });
  // API endpoint pro získání kompletní historie ve formátu CSV
  server.on("/api/history", []() {
      if (isWriting) { server.send(503, "text/plain", "Zapisuji..."); return; }
      
      // Zjistíme celkovou velikost souborů, abychom Stringu rezervovali paměť naráz
      size_t totalSize = 0;
      if (LittleFS.exists(oldFilename)) { File f = LittleFS.open(oldFilename, "r"); totalSize += f.size(); f.close(); }
      if (LittleFS.exists(filename)) { File f = LittleFS.open(filename, "r"); totalSize += f.size(); f.close(); }

      if (totalSize == 0) {
          server.send(200, "text/plain", "no_data");
          return;
      }

      String output;
      output.reserve(totalSize + 100); // Rezervujeme paměť dopředu - zabráníme pádům

      if (LittleFS.exists(oldFilename)) {
          File fOld = LittleFS.open(oldFilename, "r");
          output += fOld.readString(); // Přečte celý soubor naráz (mnohem rychlejší)
          fOld.close();
      }
      
      if (LittleFS.exists(filename)) {
          File f = LittleFS.open(filename, "r");
          output += f.readString();
          f.close();
      }

      server.send(200, "text/plain", output);
  });
  // API endpoint pro skenování čidel (vratí seznam adres v JSONu)
  server.on("/api/scan", []() {
    String json = "[";
    byte addr[8];
    oneWire.reset_search();
    bool first = true;
    while (oneWire.search(addr)) {
      if (!first) json += ",";
      first = false;
      
      String hexAddr = "{";
      for (int i = 0; i < 8; i++) {
        hexAddr += "0x";
        if (addr[i] < 16) hexAddr += "0";
        hexAddr += String(addr[i], HEX);
        if (i < 7) hexAddr += ", ";
      }
      hexAddr += "}";

      String name = "Neznamé";
      bool known = false;
      for (int i = 0; i < 7; i++) {
        if (memcmp(addr, mojeCidla[i].adr, 8) == 0) {
          name = mojeCidla[i].name;
          known = true;
          break;
        }
      }
      json += "{\"addr\":\"" + hexAddr + "\", \"known\":" + (known ? "true" : "false") + ", \"name\":\"" + name + "\"}";
    }
    json += "]";
    server.send(200, "application/json", json);
  });
  server.begin(); // Spuštění webového serveru
  
  // Nastavení watchdogu na 30 sekund
  esp_task_wdt_init(30, true); 
  esp_task_wdt_add(NULL); // Přidání aktuálního vlákna (hlavní smyčky)
  Serial.println("Watchdog nastaven na 30s");
}

// Hlavní smyčka
void loop() {
  esp_task_wdt_reset(); // Reset watchdogu
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