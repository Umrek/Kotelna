#include <Arduino.h>

#define BLYNK_TEMPLATE_ID "TMPL4LAHFNPVx"
#define BLYNK_TEMPLATE_NAME "Kotelna"
#define BLYNK_AUTH_TOKEN "XYVXCdUY1UbCmFHoA1Fj10FPdm4MCaAR"

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "max6675.h"

// WiFi přihlašovací údaje
char ssid[] = "zdrahala_Mikrotik";
char pass[] = "mojewifi65";
char auth[] = BLYNK_AUTH_TOKEN;

// Čidla (stejné zapojení jako předtím)
#define ONE_WIRE_BUS 4
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
MAX6675 thermocouple(18, 5, 19);

BlynkTimer timer;

void posliData() {
  sensors.requestTemperatures();
  
  // První čidlo vody (index 0) -> Virtuální pin V1
  float voda1 = sensors.getTempCByIndex(0);
  Blynk.virtualWrite(V1, voda1);

  // Druhé čidlo vody (index 1) -> Virtuální pin V2
  float voda2 = sensors.getTempCByIndex(1); // TADY ZMĚNA NA 1
  Blynk.virtualWrite(V2, voda2);

  // Teplota spalin -> Virtuální pin V0
  float spaliny = thermocouple.readCelsius();
  Blynk.virtualWrite(V0, spaliny);
  
  // Výpis do monitoru (přidal jsem mezeru pro čitelnost)
  Serial.print("Voda 1: "); Serial.print(voda1);
  Serial.print(" | Voda 2: "); Serial.print(voda2);
  Serial.print(" | Spaliny: "); Serial.println(spaliny);
}

void setup() {
  Serial.begin(115200);
  sensors.begin();
  
  // Připojení k Blynku
  Blynk.begin(auth, ssid, pass);
  
  // Nastavení odesílání dat každé 2 sekundy
  timer.setInterval(2000L, posliData);
}

void loop() {
  Blynk.run();
  timer.run();
}