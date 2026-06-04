/**
 * ============================================================
 *  CoolGrid / CoolFlow  —  ESP32 + Firebase RTDB  v2.0
 *  Hardware: Schematic Sheet_1 Rev 1.0
 * ============================================================
 *
 *  ARCHITECTURE:
 *    ESP32  ──(HTTPS)──▶  Firebase Realtime DB  ◀──(HTTPS)──  PWA
 *    Writes telemetry to /coolgrid/telemetry every 5s
 *    Listens to /coolgrid/cmd for pump commands from PWA
 *
 *  LIBRARIES (install via Arduino Library Manager):
 *    - DHT sensor library          by Adafruit
 *    - Adafruit Unified Sensor     by Adafruit
 *    - OneWire                     by Paul Stoffregen
 *    - DallasTemperature           by Miles Burton
 *    - Firebase ESP Client         by Mobizt   ← important
 *
 *  NOTE: The Firebase ESP Client API surface changes between
 *  major versions. This code targets v4.x. If you install a
 *  different version, the function names may differ slightly.
 *  Check the library examples folder if compile errors appear.
 * ============================================================
 */

#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ─────────────────────────────────────────────
//  CONFIG — edit before flashing
// ─────────────────────────────────────────────
#define WIFI_SSID         "YOUR_WIFI_SSID"
#define WIFI_PASSWORD     "YOUR_WIFI_PASSWORD"

// From Firebase Console → Project Settings → "Your apps" → Web app
#define API_KEY           "YOUR_FIREBASE_API_KEY"
#define DATABASE_URL      "https://YOUR_PROJECT-default-rtdb.firebaseio.com"

// Create a user in Firebase Auth → Email/Password, then put the credentials here.
// This is what the ESP32 uses to authenticate writes.
#define USER_EMAIL        "device@coolgrid.local"
#define USER_PASSWORD     "your_strong_password"

// ─────────────────────────────────────────────
//  PINS — verify against your wiring
// ─────────────────────────────────────────────
#define PIN_DHT11         13
#define PIN_DHT22         23
#define PIN_DS18B20       22
#define PIN_ECHO          21       // HRLV-MaxSonar PW pin
#define PIN_RELAY         14

// ─────────────────────────────────────────────
//  THRESHOLDS
// ─────────────────────────────────────────────
const float CISTERN_FULL_CM     = 30.0;   // HRLV min range — see firmware notes
const float CISTERN_EMPTY_CM    = 45.0;
const float PUMP_LOW_LEVEL_PCT  = 15.0;
const float CONDENSER_HOT_C     = 35.0;
const float CONDENSER_TARGET_C  = 28.0;
const float AMBIENT_ALERT_C     = 38.0;

const int   ULTRASONIC_SAMPLES  = 7;
const float ULTRASONIC_MIN_CM   = 30.0;
const float ULTRASONIC_MAX_CM   = 500.0;

// Timing
const unsigned long SENSOR_READ_MS    = 2000;   // local sensor poll
const unsigned long FIREBASE_PUSH_MS  = 5000;   // telemetry to Firebase every 5s
const unsigned long STREAM_CHECK_MS   = 500;    // poll command stream

// ─────────────────────────────────────────────
//  GLOBALS
// ─────────────────────────────────────────────
DHT dht11(PIN_DHT11, DHT11);
DHT dht22(PIN_DHT22, DHT22);
OneWire oneWire(PIN_DS18B20);
DallasTemperature ds18b20(&oneWire);

FirebaseData fbdo;          // for telemetry writes
FirebaseData fbStream;      // dedicated for the command stream
FirebaseAuth auth;
FirebaseConfig config;

bool firebaseReady = false;

unsigned long lastSensorReadMs   = 0;
unsigned long lastFirebasePushMs = 0;
unsigned long pumpStartedMs      = 0;
unsigned long pumpRuntimeMs      = 0;

bool  pumpRunning = false;
bool  autoMode    = true;

float dht11TempC = NAN, dht11Humidity = NAN;
float dht22TempC = NAN, dht22Humidity = NAN;
float waterTempC = NAN;
float cisternPct = 0.0, cisternDistCm = 0.0;

// Track last processed command timestamp to ignore replays of the same command
unsigned long lastCmdTs = 0;

// ─────────────────────────────────────────────
//  FORWARD DECLARATIONS
// ─────────────────────────────────────────────
void connectWiFi();
void initFirebase();
void readAllSensors();
float readUltrasonicCm();
float distanceToLevelPct(float distCm);
void setPump(bool on, const char* reason);
void runAutomationLogic();
void pushTelemetry();
void handleCommandStream();
unsigned long pumpRuntimeHours();
String currentRuntimeStr();

// ═════════════════════════════════════════════
//  SETUP
// ═════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== CoolGrid Firebase v2.0 ===");

  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, HIGH);  // active-LOW relay — off at boot

  dht11.begin();
  dht22.begin();
  ds18b20.begin();

  connectWiFi();
  initFirebase();

  Serial.println("Setup complete.");
}

// ═════════════════════════════════════════════
//  LOOP
// ═════════════════════════════════════════════
void loop() {
  unsigned long now = millis();

  // Read sensors every 2s
  if (now - lastSensorReadMs >= SENSOR_READ_MS) {
    lastSensorReadMs = now;
    readAllSensors();
    runAutomationLogic();
  }

  // Push to Firebase every 5s
  if (firebaseReady && (now - lastFirebasePushMs >= FIREBASE_PUSH_MS)) {
    lastFirebasePushMs = now;
    pushTelemetry();
  }

  // Listen for incoming commands
  if (firebaseReady) handleCommandStream();
}

// ─────────────────────────────────────────────
//  WIFI
// ─────────────────────────────────────────────
void connectWiFi() {
  Serial.printf("Connecting to WiFi: %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500); Serial.print("."); tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi connected. IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi failed.");
  }
}

// ─────────────────────────────────────────────
//  FIREBASE INIT
// ─────────────────────────────────────────────
void initFirebase() {
  config.api_key      = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email     = USER_EMAIL;
  auth.user.password  = USER_PASSWORD;

  // Token refresh callback (from TokenHelper.h)
  config.token_status_callback = tokenStatusCallback;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Increase buffer sizes for stable streaming
  fbdo.setBSSLBufferSize(4096, 1024);
  fbStream.setBSSLBufferSize(4096, 1024);

  Serial.println("Waiting for Firebase auth...");
  unsigned long startMs = millis();
  while (!Firebase.ready() && (millis() - startMs) < 15000) {
    delay(200);
  }

  if (Firebase.ready()) {
    firebaseReady = true;
    Serial.println("Firebase authenticated.");

    // Begin a stream on the command path so we get push-style updates
    if (!Firebase.RTDB.beginStream(&fbStream, "/coolgrid/cmd")) {
      Serial.printf("Stream begin failed: %s\n", fbStream.errorReason().c_str());
    } else {
      Serial.println("Listening for commands at /coolgrid/cmd");
    }
  } else {
    Serial.println("Firebase auth timed out — will retry in loop.");
  }
}

// ─────────────────────────────────────────────
//  SENSORS
// ─────────────────────────────────────────────
void readAllSensors() {
  dht11TempC    = dht11.readTemperature();
  dht11Humidity = dht11.readHumidity();
  dht22TempC    = dht22.readTemperature();
  dht22Humidity = dht22.readHumidity();

  ds18b20.requestTemperatures();
  waterTempC = ds18b20.getTempCByIndex(0);
  if (waterTempC == DEVICE_DISCONNECTED_C) waterTempC = NAN;

  cisternDistCm = readUltrasonicCm();
  cisternPct    = distanceToLevelPct(cisternDistCm);

  Serial.printf("[Sensors] DHT11:%.1f°C %.0f%% | DHT22:%.1f°C | Water:%.2f°C | Cistern:%.0f%% | Pump:%s\n",
    dht11TempC, dht11Humidity, dht22TempC, waterTempC, cisternPct, pumpRunning ? "ON" : "OFF");
}

float readUltrasonicCm() {
  // HRLV-MaxSonar PW: 1us = 1mm. Free-running ~10Hz.
  // Take N samples, reject out-of-range, return median.
  float readings[ULTRASONIC_SAMPLES];
  int validCount = 0;

  for (int i = 0; i < ULTRASONIC_SAMPLES; i++) {
    long duration = pulseIn(PIN_ECHO, HIGH, 200000);
    if (duration == 0) continue;
    float distCm = duration / 10.0;
    if (distCm < ULTRASONIC_MIN_CM || distCm > ULTRASONIC_MAX_CM) continue;
    readings[validCount++] = distCm;
    delay(30);
  }

  if (validCount == 0) {
    Serial.println("HRLV: no valid samples");
    return CISTERN_EMPTY_CM;
  }

  // Insertion sort
  for (int i = 1; i < validCount; i++) {
    float key = readings[i]; int j = i - 1;
    while (j >= 0 && readings[j] > key) { readings[j+1] = readings[j]; j--; }
    readings[j+1] = key;
  }
  return readings[validCount / 2];
}

float distanceToLevelPct(float distCm) {
  float p = ((CISTERN_EMPTY_CM - distCm) / (CISTERN_EMPTY_CM - CISTERN_FULL_CM)) * 100.0f;
  return constrain(p, 0.0f, 100.0f);
}

// ─────────────────────────────────────────────
//  PUMP CONTROL
// ─────────────────────────────────────────────
void setPump(bool on, const char* reason) {
  if (on && cisternPct < PUMP_LOW_LEVEL_PCT) {
    Serial.println("Pump BLOCKED: cistern too low");
    on = false;
  }
  if (on == pumpRunning) return;

  if (on)                          pumpStartedMs = millis();
  else if (pumpStartedMs > 0)    { pumpRuntimeMs += millis() - pumpStartedMs; pumpStartedMs = 0; }

  pumpRunning = on;
  digitalWrite(PIN_RELAY, on ? LOW : HIGH);
  Serial.printf("Pump %s — %s\n", on ? "ON" : "OFF", reason);
}

void runAutomationLogic() {
  if (!autoMode) return;
  if (!isnan(waterTempC) && waterTempC >= CONDENSER_HOT_C   && !pumpRunning) setPump(true,  "condenser_hot");
  if (!isnan(waterTempC) && waterTempC <= CONDENSER_TARGET_C && pumpRunning) setPump(false, "target_reached");
  if (cisternPct < PUMP_LOW_LEVEL_PCT && pumpRunning) setPump(false, "cistern_low");
}

unsigned long currentRuntimeMs() {
  unsigned long ms = pumpRuntimeMs;
  if (pumpRunning && pumpStartedMs > 0) ms += millis() - pumpStartedMs;
  return ms;
}

// ─────────────────────────────────────────────
//  FIREBASE PUSH  — telemetry
// ─────────────────────────────────────────────
void pushTelemetry() {
  // Build JSON
  FirebaseJson json;
  json.set("device_id",          "coolgrid-esp32-01");
  json.set("uptime_ms",          (int)millis());
  json.set("dht11_temp",         isnan(dht11TempC)    ? -99.0 : dht11TempC);
  json.set("dht11_hum",          isnan(dht11Humidity) ? -99.0 : dht11Humidity);
  json.set("dht22_temp",         isnan(dht22TempC)    ? -99.0 : dht22TempC);
  json.set("dht22_hum",          isnan(dht22Humidity) ? -99.0 : dht22Humidity);
  json.set("water_temp",         isnan(waterTempC)    ? -99.0 : waterTempC);
  json.set("cistern_pct",        cisternPct);
  json.set("cistern_cm",         cisternDistCm);
  json.set("pump_on",            pumpRunning);
  json.set("auto_mode",          autoMode);
  json.set("pump_runtime_hours", currentRuntimeMs() / 3600000.0);

  // Use setJSON to overwrite the whole telemetry node atomically
  if (Firebase.RTDB.setJSON(&fbdo, "/coolgrid/telemetry", &json)) {
    Serial.println("[Firebase] telemetry pushed");
  } else {
    Serial.printf("[Firebase] push failed: %s\n", fbdo.errorReason().c_str());
  }
}

// ─────────────────────────────────────────────
//  FIREBASE STREAM  — incoming commands
// ─────────────────────────────────────────────
void handleCommandStream() {
  if (!Firebase.RTDB.readStream(&fbStream)) {
    Serial.printf("[Stream] error: %s\n", fbStream.errorReason().c_str());
    return;
  }
  if (!fbStream.streamAvailable()) return;

  // Stream gives us the new payload as a JSON string or per-field update
  // We expect a JSON object: { "cmd": "ON|OFF|AUTO", "ts": <ms>, "src": "pwa" }
  Serial.printf("[Stream] event=%s type=%s path=%s\n",
                fbStream.eventType().c_str(),
                fbStream.dataType().c_str(),
                fbStream.dataPath().c_str());

  String cmd = "";
  unsigned long ts = 0;

  if (fbStream.dataType() == "json") {
    FirebaseJson* j = fbStream.jsonObjectPtr();
    FirebaseJsonData out;
    if (j->get(out, "cmd"))  cmd = out.stringValue;
    if (j->get(out, "ts"))   ts  = out.intValue;
  } else if (fbStream.dataType() == "string") {
    cmd = fbStream.stringData();
  }

  if (cmd.length() == 0) return;
  if (ts != 0 && ts == lastCmdTs) return;   // duplicate; ignore
  lastCmdTs = ts;

  cmd.toUpperCase();
  Serial.printf("[CMD] %s\n", cmd.c_str());

  if (cmd == "ON")        { autoMode = false; setPump(true,  "pwa_command"); }
  else if (cmd == "OFF")  { autoMode = false; setPump(false, "pwa_command"); }
  else if (cmd == "AUTO") { autoMode = true;  Serial.println("Auto mode re-enabled"); }
}
