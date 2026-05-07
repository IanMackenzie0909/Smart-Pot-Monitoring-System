/***************************************************
 * Smart Plant - ESP8266 + Soil Sensor + Relay + Blynk
 * Board: NodeMCU 1.0 (ESP-12E Module)
 ***************************************************/

// Debug print
#define BLYNK_PRINT Serial

// === Blynk Template & Auth ===
#define BLYNK_TEMPLATE_ID "your_template_id"  // Replace this with your Template ID from Blynk Cloud
#define BLYNK_TEMPLATE_NAME "your_template_name"
#define BLYNK_AUTH_TOKEN "your_blynk_auth_token"

// === Wi-Fi Settings (replace with your AP) ===
char auth[] = BLYNK_AUTH_TOKEN;
const char* ssid = "your_wifi_ssid";
const char* pass = "your_wifi_password";

#include <ESP8266WiFi.h>
#include <BlynkSimpleEsp8266.h>

// === Hardware Pin Definitions ===
#define SOIL_PIN   A0        // Connect the soil moisture sensor A0 analog output here
#define RELAY_PIN  D1        // Relay IN pin; change this if you use another pin

// === Relay Trigger Logic ===
// Based on current testing: HIGH = pump ON, LOW = pump OFF
#define RELAY_ON   HIGH
#define RELAY_OFF  LOW

// === Auto-Watering Parameters (adjust as needed) ===
const unsigned long PUMP_DURATION_MS = 3000;            // Maximum watering duration per run: 5 seconds
const unsigned long MIN_INTERVAL_MS  = 10UL * 1 * 1000; // Minimum interval between watering runs: 10 minutes (can be shorter for testing)
const int HYSTERESIS_PCT = 10;                          // Moisture hysteresis: stop threshold = low threshold + 10%

// === Soil Moisture Calibration (A0 reading: 0-1023) ===
// Recommended soilDryAdc value is 1023 (ESP8266 analogRead usually maxes out at 1023)
int soilDryAdc  = 1023;  // A0 value in air or very dry soil
int soilWetAdc  = 430;   // A0 value in water or very wet soil

// ===================================================
// Blynk virtual pins (remapped to avoid V1/V2/V3)
// ===================================================
#define VPIN_SOIL        10   // V10: Soil Moisture %
#define VPIN_THRESHOLD   11   // V11: Slider -> Auto LOW threshold %
#define VPIN_AUTOMODE    12   // V12: Switch -> Auto mode ON/OFF
#define VPIN_MANUAL      13   // V13: Button -> Manual pump (push)
#define VPIN_PUMP_STATE  14   // V14: LED/Value -> Pump state (1=ON, 0=OFF)

BlynkTimer timer;

bool autoMode = true;
int  autoThreshold = 40;

bool pumpOn = false;
unsigned long pumpStartTime = 0;

// Use the last pump stop time as the interval reference
unsigned long lastPumpEndTime = 0;   // 0 = watering has not run yet
int lastMoisturePct = 0;

// Convert the A0 reading to 0-100% (0 = driest, 100 = wettest)
int adcToPercent(int adcRaw)
{
  if (adcRaw < soilWetAdc) adcRaw = soilWetAdc;
  if (adcRaw > soilDryAdc) adcRaw = soilDryAdc;

  int pct = map(adcRaw, soilDryAdc, soilWetAdc, 0, 100);
  if (pct < 0)   pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

// Start the pump for one watering run
void startPumpOnce()
{
  unsigned long now = millis();

  // Enforce the interval based on the last pump stop time
  if (lastPumpEndTime != 0 && (now - lastPumpEndTime < MIN_INTERVAL_MS)) {
    Serial.println(F("[PUMP] Skip: interval too short."));
    return;
  }

  Serial.println(F("[PUMP] ON"));
  digitalWrite(RELAY_PIN, RELAY_ON);
  pumpOn = true;
  pumpStartTime = now;

  // Report the pump state to V14 (do not write back to the manual button)
  Blynk.virtualWrite(VPIN_PUMP_STATE, 1);
}

// Stop the pump
void stopPump()
{
  if (!pumpOn) return;

  Serial.println(F("[PUMP] OFF"));
  digitalWrite(RELAY_PIN, RELAY_OFF);
  pumpOn = false;

  // Record the pump stop time for MIN_INTERVAL
  lastPumpEndTime = millis();

  // Report the pump state to V14
  Blynk.virtualWrite(VPIN_PUMP_STATE, 0);
}

// Periodically read soil moisture, upload it, and evaluate auto-watering
void readSoilAndControl()
{
  int adcRaw = analogRead(SOIL_PIN);
  int moisturePct = adcToPercent(adcRaw);
  lastMoisturePct = moisturePct;

  Serial.print(F("Soil ADC = "));
  Serial.print(adcRaw);
  Serial.print(F("  -> Moisture = "));
  Serial.print(moisturePct);
  Serial.println(F("%"));

  // Upload to Blynk: V10
  Blynk.virtualWrite(VPIN_SOIL, moisturePct);

  // Auto-watering only handles turning the pump on
  if (autoMode && !pumpOn) {
    if (moisturePct < autoThreshold) {
      Serial.println(F("[AUTO] Soil too dry, start pump."));
      startPumpOnce();
    }
  }
}

// Stop the pump when the duration is reached or the soil is wet enough
void pumpSafetyCheck()
{
  if (!pumpOn) return;

  unsigned long now = millis();
  bool timeUp = (now - pumpStartTime >= PUMP_DURATION_MS);

  int highThreshold = autoThreshold + HYSTERESIS_PCT;
  if (highThreshold > 100) highThreshold = 100;
  bool wetEnough = (lastMoisturePct >= highThreshold);

  if (timeUp || wetEnough) {
    if (timeUp)    Serial.println(F("[PUMP] Duration reached, stop pump."));
    if (wetEnough) Serial.println(F("[PUMP] Soil moisture high enough, stop pump."));
    stopPump();
  }
}

// ===================== Blynk Callbacks =====================

// V11: Slider sets the auto-watering low threshold
BLYNK_WRITE(VPIN_THRESHOLD)
{
  autoThreshold = param.asInt();
  Serial.print(F("[Blynk] New autoThreshold = "));
  Serial.println(autoThreshold);
}

// V12: Switch toggles auto mode ON/OFF
BLYNK_WRITE(VPIN_AUTOMODE)
{
  autoMode = (param.asInt() == 1);
  Serial.print(F("[Blynk] Auto mode = "));
  Serial.println(autoMode ? "ON" : "OFF");
}

// V13: Button manually starts one pump run (Push mode is recommended)
BLYNK_WRITE(VPIN_MANUAL)
{
  int btn = param.asInt();
  if (btn == 1) {
    Serial.println(F("[Blynk] Manual pump button pressed."));
    startPumpOnce();
  }
  // Releasing the button does not force the pump off; pumpSafetyCheck controls it
}

BLYNK_CONNECTED()
{
  // Sync current settings to the app after connection
  Blynk.virtualWrite(VPIN_THRESHOLD, autoThreshold);
  Blynk.virtualWrite(VPIN_AUTOMODE, autoMode ? 1 : 0);
  Blynk.virtualWrite(VPIN_PUMP_STATE, pumpOn ? 1 : 0);
}

// =======================================================

void setup()
{
  Serial.begin(9600);
  delay(100);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF);

  // Connect to Blynk Cloud
  Blynk.begin(auth, ssid, pass);

  // Update moisture and decide whether to start the pump every 5 seconds
  timer.setInterval(5000L, readSoilAndControl);
  // Check whether to stop the pump every 200 ms
  timer.setInterval(200L, pumpSafetyCheck);

  Serial.println(F("Smart Plant system started (Blynk + local logic)."));
}

void loop()
{
  Blynk.run();
  timer.run();
}
