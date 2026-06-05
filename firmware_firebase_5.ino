
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ─────────────────────────────────────────────
//  CONFIG — edit before flashing
// ─────────────────────────────────────────────
#define WIFI_SSID         "SL"
#define WIFI_PASSWORD     "11223344"

#define API_KEY           "AIzaSyDTwrr30zUl4Qo02XO-Rgd9aefhSGPBFCU"
#define DATABASE_URL      "https://coolgrid-prototype-default-rtdb.asia-southeast1.firebasedatabase.app/"


#define USER_EMAIL        "device@coolgrid.local"
#define USER_PASSWORD     "061003070725"

// ─────────────────────────────────────────────
//  PINS — verify against your wiring
// ─────────────────────────────────────────────
#define PIN_DS18B20_WATER      22   // Water temperature sensor
#define PIN_DS18B20_CONDENSER  23   // Condenser temperature sensor

#define _stringify(x) #x
#define stringify(x)  _stringify(x)
#define PIN_ECHO          21       // HRLV-MaxSonar PW pin
#define PIN_RELAY         26

// ─────────────────────────────────────────────
//  MISTING CYCLE CONFIG
// ─────────────────────────────────────────────
const unsigned long SPRAY_DURATION_MS = 3000;   // 3 seconds spray
const unsigned long PAUSE_DURATION_MS = 2000;   // 2 seconds pause

// Relay logic — most relay modules are active-LOW (LOW = on)
// Flip these if yours is active-HIGH
#define RELAY_ON   LOW
#define RELAY_OFF  HIGH

// ─────────────────────────────────────────────
//  DS18B20 SENSOR ASSIGNMENT
// ─────────────────────────────────────────────
//  Each sensor is on its own dedicated OneWire pin — no index ambiguity.
//    PIN_DS18B20_WATER     (22) → water temperature
//    PIN_DS18B20_CONDENSER (23) → condenser temperature

// ─────────────────────────────────────────────
//  THRESHOLDS
// ─────────────────────────────────────────────
const float CISTERN_FULL_CM     = 3.5;
const float CISTERN_EMPTY_CM    = 14.0;
const float PUMP_LOW_LEVEL_PCT  = 10.0;
const float WATER_OVERTEMP_C    = 45.0;
const float COMPRESSOR_HOT_C    = 45.0;   // compressor "warm" threshold
const float COMPRESSOR_CRIT_C   = 60.0;   // compressor "hot" threshold (alert)

const int   ULTRASONIC_SAMPLES  = 7;
// Tank operating range is 3.5cm (full) to 14cm (empty). The previous 30cm
// minimum was rejecting every valid reading — that's why the dashboard was
// stuck at ~30cm. Widened to 1–30cm to cover the tank plus some margin.
const float ULTRASONIC_MIN_CM   = 1.0;
const float ULTRASONIC_MAX_CM   = 30.0;

// Timing
const unsigned long SENSOR_READ_MS   = 2000;
const unsigned long FIREBASE_PUSH_MS = 5000;

// ─────────────────────────────────────────────
//  MISTING STATE MACHINE
// ─────────────────────────────────────────────
enum MistState { MIST_IDLE, MIST_SPRAYING, MIST_PAUSED, MIST_BLOCKED };
MistState mistState = MIST_IDLE;
unsigned long mistStateEnteredMs = 0;
unsigned long cycleCount         = 0;   // total spray cycles since boot
unsigned long totalSprayMs       = 0;   // accumulated spray time

// Pump mode: AUTO cycles, MANUAL_ON sprays continuously, MANUAL_OFF stops
enum PumpMode { MODE_AUTO, MODE_MANUAL_ON, MODE_MANUAL_OFF };
PumpMode pumpMode = MODE_AUTO;

// ─────────────────────────────────────────────
//  COOLING EFFECTIVENESS (AI-style analytics)
// ─────────────────────────────────────────────
//  Measures the actual cooling effect each spray cycle has on the
//  compressor (heat chamber) temperature, builds rolling statistics,
//  and flags cycles that perform anomalously poorly.
// ─────────────────────────────────────────────
#define EFFECTIVENESS_WINDOW 20      // rolling stats over last 20 cycles
#define ANOMALY_MIN_SAMPLES  5       // need this many cycles before flagging
const float ANOMALY_THRESHOLD_C = 0.5;  // °C below average = anomalous cycle

float cycleStartCompTemp     = NAN;       // compressor temp at start of current cycle
float recentDeltas[EFFECTIVENESS_WINDOW] = {0};
int   deltaWriteIdx          = 0;
int   deltasRecorded         = 0;
float lastCycleDelta         = 0;         // most recent cycle's cooling delta (°C)
float avgCycleDelta          = 0;         // rolling avg over EFFECTIVENESS_WINDOW
float bestCycleDelta         = 0;         // best single-cycle delta ever
float cumulativeCoolingC     = 0;         // total degrees of cooling delivered
float sessionStartCompTemp   = NAN;       // compressor temp when AUTO mode last started
bool  lastCycleAnomalous     = false;     // flag for dashboard

// ─────────────────────────────────────────────
//  GLOBALS
// ─────────────────────────────────────────────
OneWire oneWireWater(PIN_DS18B20_WATER);
OneWire oneWireCondenser(PIN_DS18B20_CONDENSER);
DallasTemperature sensorWater(&oneWireWater);
DallasTemperature sensorCondenser(&oneWireCondenser);

FirebaseData fbdo;
FirebaseData fbStream;
FirebaseAuth auth;
FirebaseConfig config;

bool firebaseReady = false;

unsigned long lastSensorReadMs   = 0;
unsigned long lastFirebasePushMs = 0;

float waterTempC      = NAN;
float compressorTempC = NAN;
float cisternPct      = 0.0;
float cisternDistCm   = 0.0;

unsigned long lastCmdTs = 0;

// ─────────────────────────────────────────────
//  FORWARD DECLARATIONS
// ─────────────────────────────────────────────
void connectWiFi();
void initFirebase();
void readSensors();
float readUltrasonicCm();
float distanceToLevelPct(float distCm);
void setRelay(bool on);
void updateMistingStateMachine();
void pushTelemetry();
void handleCommandStream();
const char* mistStateStr();

// ═════════════════════════════════════════════
//  SETUP
// ═════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== CoolGrid Firebase v2.1 (misting) ===");

  pinMode(PIN_RELAY, OUTPUT);
  setRelay(false);

  sensorWater.begin();
  sensorCondenser.begin();

  // Diagnostic: confirm both DS18B20 sensors are detected
  int waterCount     = sensorWater.getDeviceCount();
  int condenserCount = sensorCondenser.getDeviceCount();
  Serial.printf("DS18B20 water     (pin %d): %d sensor(s) detected\n", PIN_DS18B20_WATER,     waterCount);
  Serial.printf("DS18B20 condenser (pin %d): %d sensor(s) detected\n", PIN_DS18B20_CONDENSER, condenserCount);
  if (waterCount < 1) {
    Serial.println("WARN: no water temperature sensor found on pin " stringify(PIN_DS18B20_WATER) ". Check wiring.");
  }
  if (condenserCount < 1) {
    Serial.println("WARN: no condenser temperature sensor found on pin " stringify(PIN_DS18B20_CONDENSER) ". Check wiring.");
  }

  connectWiFi();
  initFirebase();

  Serial.println("Setup complete. AUTO mode misting will begin on next telemetry cycle.");
}

// ═════════════════════════════════════════════
//  LOOP
// ═════════════════════════════════════════════
void loop() {
  unsigned long now = millis();

  // Read sensors every 2s
  if (now - lastSensorReadMs >= SENSOR_READ_MS) {
    lastSensorReadMs = now;
    readSensors();
  }

  // State machine runs every loop iteration for accurate timing
  updateMistingStateMachine();

  // Push to Firebase every 5s
  if (firebaseReady && (now - lastFirebasePushMs >= FIREBASE_PUSH_MS)) {
    lastFirebasePushMs = now;
    pushTelemetry();
  }

  if (firebaseReady) handleCommandStream();
}

// ─────────────────────────────────────────────
//  WIFI
// ─────────────────────────────────────────────
void connectWiFi() {
  Serial.println("Scanning...");
   int n = WiFi.scanNetworks();
   for (int i = 0; i < n; i++) {
     Serial.printf("  %s  RSSI=%d  ch=%d\n",
       WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
   }
  // WIFI_STA mode is more compatible with phone hotspots than WIFI_AP_STA.
  // AP+STA can cause conflicts when the ESP32 tries to reach internet via hotspot.
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // disable modem sleep — keeps connection stable
  WiFi.setAutoReconnect(true);   // auto-reconnect if hotspot drops briefly

  const int MAX_ATTEMPTS = 3;
  for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
    Serial.printf("WiFi connect attempt %d/%d to: %s\n", attempt, MAX_ATTEMPTS, WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 40) {
      delay(500); Serial.print("."); tries++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\nWiFi connected. IP: %s  RSSI: %d dBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      return;  // success — exit early
    }

    Serial.printf("\nAttempt %d failed (status %d). ", attempt, WiFi.status());
    if (attempt < MAX_ATTEMPTS) {
      WiFi.disconnect(true);
      delay(2000);
      Serial.println("Retrying...");
    }
  }

  Serial.println("WiFi failed after all attempts. Firebase will not be available.");
}

// ─────────────────────────────────────────────
//  FIREBASE INIT
// ─────────────────────────────────────────────
void initFirebase() {
  config.api_key      = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email     = USER_EMAIL;
  auth.user.password  = USER_PASSWORD;
  config.token_status_callback = tokenStatusCallback;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Larger SSL buffers help on slow hotspot connections (phone tethering).
  fbdo.setBSSLBufferSize(8192, 2048);
  fbStream.setBSSLBufferSize(8192, 2048);

  // Retry Firebase auth up to 3 times with 30s each — phone hotspots can
  // be slow to allow outbound SSL on first connect.
  const int FB_MAX_ATTEMPTS = 3;
  const unsigned long FB_TIMEOUT_MS = 30000;

  for (int attempt = 1; attempt <= FB_MAX_ATTEMPTS; attempt++) {
    Serial.printf("Firebase auth attempt %d/%d (timeout %lus)...\n",
                  attempt, FB_MAX_ATTEMPTS, FB_TIMEOUT_MS / 1000);

    unsigned long startMs = millis();
    while (!Firebase.ready() && (millis() - startMs) < FB_TIMEOUT_MS) {
      delay(200);
    }

    if (Firebase.ready()) {
      firebaseReady = true;
      Serial.println("Firebase authenticated.");
      if (!Firebase.RTDB.beginStream(&fbStream, "/coolgrid/cmd")) {
        Serial.printf("Stream begin failed: %s\n", fbStream.errorReason().c_str());
      } else {
        Serial.println("Listening for commands at /coolgrid/cmd");
      }
      return;  // success — exit early
    }

    Serial.printf("Firebase auth attempt %d timed out.", attempt);
    if (attempt < FB_MAX_ATTEMPTS) {
      Serial.println(" Retrying...");
      delay(3000);
    }
  }

  Serial.println("Firebase auth failed after all attempts. Running without cloud sync.");
}

// ─────────────────────────────────────────────
//  SENSORS
// ─────────────────────────────────────────────
void readSensors() {
  sensorWater.requestTemperatures();
  sensorCondenser.requestTemperatures();

  waterTempC = sensorWater.getTempCByIndex(0);
  if (waterTempC == DEVICE_DISCONNECTED_C) waterTempC = NAN;

  compressorTempC = sensorCondenser.getTempCByIndex(0);
  if (compressorTempC == DEVICE_DISCONNECTED_C) compressorTempC = NAN;

  cisternDistCm = readUltrasonicCm();
  cisternPct    = distanceToLevelPct(cisternDistCm);

  Serial.printf("[Sensors] Water:%.2f°C | Compressor:%.2f°C | Cistern:%.0f%% | Mist:%s | Cycles:%lu\n",
    waterTempC, compressorTempC, cisternPct, mistStateStr(), cycleCount);
}

float readUltrasonicCm() {
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

  if (validCount == 0) return CISTERN_EMPTY_CM;

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
//  RELAY  (raw control — does not log)
// ─────────────────────────────────────────────
void setRelay(bool on) {
  digitalWrite(PIN_RELAY, on ? RELAY_ON : RELAY_OFF);
}

// ─────────────────────────────────────────────
//  COOLING EFFECTIVENESS — record a completed cycle
// ─────────────────────────────────────────────
//  Called at the end of each AUTO mode cycle (spray + pause).
//  Updates rolling statistics and detects anomalies.
//  Positive delta = compressor cooled. Negative = it heated.
// ─────────────────────────────────────────────
void recordCycleDelta(float delta) {
  if (isnan(delta) || delta > 20.0 || delta < -20.0) {
    Serial.println("[Effectiveness] cycle delta rejected (out of plausible range)");
    return;
  }

  lastCycleDelta = delta;
  recentDeltas[deltaWriteIdx] = delta;
  deltaWriteIdx = (deltaWriteIdx + 1) % EFFECTIVENESS_WINDOW;
  if (deltasRecorded < EFFECTIVENESS_WINDOW) deltasRecorded++;

  if (delta > bestCycleDelta) bestCycleDelta = delta;
  if (delta > 0) cumulativeCoolingC += delta;

  float sum = 0;
  for (int i = 0; i < deltasRecorded; i++) sum += recentDeltas[i];
  avgCycleDelta = sum / deltasRecorded;

  // Anomaly: cycle noticeably worse than recent avg
  lastCycleAnomalous = (deltasRecorded >= ANOMALY_MIN_SAMPLES) &&
                       (delta < (avgCycleDelta - ANOMALY_THRESHOLD_C));

  Serial.printf("[Effectiveness] delta=%.2f°C  avg=%.2f°C  best=%.2f°C  cumulative=%.1f°C  %s\n",
    delta, avgCycleDelta, bestCycleDelta, cumulativeCoolingC,
    lastCycleAnomalous ? "ANOMALOUS" : "");
}

void resetEffectivenessSession() {
  sessionStartCompTemp = compressorTempC;
  lastCycleAnomalous = false;
}

// ─────────────────────────────────────────────
//  MISTING STATE MACHINE
// ─────────────────────────────────────────────
//  State transitions:
//    IDLE → SPRAYING       (AUTO mode active + cistern OK)
//    SPRAYING → PAUSED     (after SPRAY_DURATION_MS)
//    PAUSED → SPRAYING     (after PAUSE_DURATION_MS), increment cycleCount
//    any → IDLE            (MANUAL_OFF set, or AUTO toggled off)
//    any → BLOCKED         (cistern below safety level OR water overtemp)
//    BLOCKED → IDLE        (condition cleared, returns to normal state machine)
//    any → continuously ON (MANUAL_ON — relay stays on, state shown as SPRAYING)
// ─────────────────────────────────────────────
void enterState(MistState newState) {
  if (newState == mistState) return;
  mistState = newState;
  mistStateEnteredMs = millis();
}

void updateMistingStateMachine() {
  unsigned long now = millis();
  unsigned long inState = now - mistStateEnteredMs;

  // Safety overrides DISABLED — sonar readings are not yet trusted, so we
  // don't want a bad cistern_pct reading to falsely block the pump. Re-enable
  // once the level sensor is validated.
  //
  // bool cisternLow  = (cisternPct < PUMP_LOW_LEVEL_PCT);
  // bool waterTooHot = (!isnan(waterTempC) && waterTempC >= WATER_OVERTEMP_C);
  //
  // if (cisternLow || waterTooHot) {
  //   setRelay(false);
  //   enterState(MIST_BLOCKED);
  //   return;
  // }

  // Manual modes
  if (pumpMode == MODE_MANUAL_OFF) {
    setRelay(false);
    enterState(MIST_IDLE);
    return;
  }

  if (pumpMode == MODE_MANUAL_ON) {
    // Continuous spray — relay always on, no cycling
    setRelay(true);
    enterState(MIST_SPRAYING);
    return;
  }

  // AUTO mode — run spray/pause cycle
  switch (mistState) {
    case MIST_IDLE:
    case MIST_BLOCKED:
      // Beginning the first cycle of a new session
      cycleStartCompTemp = compressorTempC;   // snapshot for delta calc
      if (isnan(sessionStartCompTemp)) sessionStartCompTemp = compressorTempC;
      setRelay(true);
      enterState(MIST_SPRAYING);
      break;

    case MIST_SPRAYING:
      setRelay(true);
      if (inState >= SPRAY_DURATION_MS) {
        totalSprayMs += SPRAY_DURATION_MS;
        setRelay(false);
        enterState(MIST_PAUSED);
      }
      break;

    case MIST_PAUSED:
      setRelay(false);
      if (inState >= PAUSE_DURATION_MS) {
        // Cycle complete — compute cooling delta now that the pause is done
        if (!isnan(cycleStartCompTemp) && !isnan(compressorTempC)) {
          float delta = cycleStartCompTemp - compressorTempC;
          recordCycleDelta(delta);
        }
        cycleCount++;
        cycleStartCompTemp = compressorTempC;   // arm next cycle
        setRelay(true);
        enterState(MIST_SPRAYING);
      }
      break;
  }
}

const char* mistStateStr() {
  switch (mistState) {
    case MIST_IDLE:     return "IDLE";
    case MIST_SPRAYING: return "SPRAYING";
    case MIST_PAUSED:   return "PAUSED";
    case MIST_BLOCKED:  return "BLOCKED";
  }
  return "UNKNOWN";
}

const char* pumpModeStr() {
  switch (pumpMode) {
    case MODE_AUTO:       return "AUTO";
    case MODE_MANUAL_ON:  return "ON";
    case MODE_MANUAL_OFF: return "OFF";
  }
  return "UNKNOWN";
}

// ─────────────────────────────────────────────
//  FIREBASE PUSH
// ─────────────────────────────────────────────
void pushTelemetry() {
  FirebaseJson json;
  json.set("device_id",          "coolgrid-esp32-01");
  json.set("uptime_ms",          (int)millis());
  json.set("water_temp",         isnan(waterTempC)      ? -99.0 : waterTempC);
  json.set("compressor_temp",    isnan(compressorTempC) ? -99.0 : compressorTempC);
  json.set("cistern_pct",        cisternPct);
  json.set("cistern_cm",         cisternDistCm);
  json.set("mist_state",         mistStateStr());
  json.set("pump_mode",          pumpModeStr());
  json.set("cycle_count",        (int)cycleCount);
  json.set("total_spray_seconds", (int)(totalSprayMs / 1000));
  // Convenience flag for the PWA (any state where relay is energised)
  bool sprayingNow = (mistState == MIST_SPRAYING);
  json.set("is_spraying",        sprayingNow);
  json.set("auto_mode",          pumpMode == MODE_AUTO);

  // Cooling effectiveness (AI analytics)
  json.set("eff_last_delta",       lastCycleDelta);
  json.set("eff_avg_delta",        avgCycleDelta);
  json.set("eff_best_delta",       bestCycleDelta);
  json.set("eff_cumulative",       cumulativeCoolingC);
  json.set("eff_cycles_measured",  deltasRecorded);
  json.set("eff_anomalous",        lastCycleAnomalous);
  json.set("eff_session_start_c",  isnan(sessionStartCompTemp) ? -99.0 : sessionStartCompTemp);

  // Recent deltas (last 10, oldest first) — for the bar chart in the dashboard.
  // Build a JSON array from the rolling buffer in chronological order.
  FirebaseJsonArray history;
  int n = min(deltasRecorded, 10);
  int startIdx = (deltaWriteIdx - n + EFFECTIVENESS_WINDOW) % EFFECTIVENESS_WINDOW;
  for (int i = 0; i < n; i++) {
    history.add(recentDeltas[(startIdx + i) % EFFECTIVENESS_WINDOW]);
  }
  json.set("eff_history", history);

  if (Firebase.RTDB.setJSON(&fbdo, "/coolgrid/telemetry", &json)) {
    Serial.println("[Firebase] telemetry pushed");
  } else {
    Serial.printf("[Firebase] push failed: %s\n", fbdo.errorReason().c_str());
  }
}

// ─────────────────────────────────────────────
//  FIREBASE STREAM — incoming commands
// ─────────────────────────────────────────────
void handleCommandStream() {
  if (!Firebase.RTDB.readStream(&fbStream)) {
    Serial.printf("[Stream] error: %s\n", fbStream.errorReason().c_str());
    return;
  }
  if (!fbStream.streamAvailable()) return;

  String cmd = "";
  unsigned long ts = 0;

  if (fbStream.dataType() == "json") {
    FirebaseJson* j = fbStream.jsonObjectPtr();
    FirebaseJsonData out;
    if (j->get(out, "cmd")) cmd = out.stringValue;
    if (j->get(out, "ts"))  ts  = out.intValue;
  } else if (fbStream.dataType() == "string") {
    cmd = fbStream.stringData();
  }

  if (cmd.length() == 0) return;
  if (ts != 0 && ts == lastCmdTs) return;   // duplicate
  lastCmdTs = ts;

  cmd.toUpperCase();
  Serial.printf("[CMD] %s\n", cmd.c_str());

  if (cmd == "ON") {
    pumpMode = MODE_MANUAL_ON;
    Serial.println("Misting: MANUAL ON (continuous spray)");
  } else if (cmd == "OFF") {
    pumpMode = MODE_MANUAL_OFF;
    Serial.println("Misting: MANUAL OFF");
  } else if (cmd == "AUTO") {
    pumpMode = MODE_AUTO;
    enterState(MIST_IDLE);  // restart the cycle cleanly
    resetEffectivenessSession();
    Serial.println("Misting: AUTO (3s spray / 2s pause cycle)");
  }
}
