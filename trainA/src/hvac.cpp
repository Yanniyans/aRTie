#include <Arduino.h>
#include "hvac.h"

#include "DHT.h"

// ==========================================
// 1. PIN DEFINITIONS & HARDWARE CONFIG
// ==========================================
#define DHTPIN     14
#define DHTTYPE    DHT22

#define PWMA_PIN   18   // TB6612FNG PWM Speed Control
#define AIN1_PIN   19   // TB6612FNG Direction 1
#define AIN2_PIN   21   // TB6612FNG Direction 2

// --- FUTURE SENSOR PINS (reserve these now) ---
// #define BREAK_BEAM_PIN  34   // IR Receiver (INPUT only GPIO)
// #define LOAD_CELL_DOUT  35   // HX711 Data
// #define LOAD_CELL_SCK   32   // HX711 Clock

// ESP32 PWM Settings
const int PWM_FREQ       = 5000;  // 5 kHz
const int PWM_RESOLUTION = 8;     // 8-bit (0-255)

const int PWM_CHANNEL = 0;

static int simulatedOccupancy = 0; // Start at 0% occupancy on boot


// ==========================================
// 2. FAN SPEED CONSTANTS
// ==========================================
const int SPEED_OFF  = 0;
const int SPEED_LOW  = 85;   // ~33%
const int SPEED_MED  = 170;  // ~66%
const int SPEED_HIGH = 255;  // 100%

const int KICKSTART_MS = 150; // Full-power pulse on OFF -> ON transition

// ==========================================
// 3. OCCUPANCY TIERS & COMFORT SETPOINTS
// ==========================================
// As occupancy rises, comfort targets tighten and the fan works harder
// to maintain them. Each tier activates when occupancy >= occupancyMin.
struct ComfortSetpoint {
  int   occupancyMin;  // % lower bound for this tier
  float targetTemp;    // Degrees C — fan tries to stay at or below this
  float targetHum;     // % — fan tries to stay at or below this
  const char* label;
};

const ComfortSetpoint SETPOINTS[] = {
  {  0, 27.0, 65.0, "LOW OCCUPANCY"    },  // Few people, relaxed target
  { 40, 25.0, 60.0, "MEDIUM OCCUPANCY" },  // Standard comfort
  { 70, 23.0, 55.0, "HIGH OCCUPANCY"   },  // Packed room, aggressive cooling
};
const int NUM_SETPOINTS = 3;

// Deviation bands — how far ABOVE the setpoint before escalating fan speed.
// Example at MEDIUM OCCUPANCY (target 25.0C):
//   t = 26.0 -> deviation 1.0 -> MEDIUM speed
//   t = 27.5 -> deviation 2.5 -> HIGH speed
//   t = 24.0 -> deviation -1.0 (below target) -> LOW speed
const float DEVIATION_MED  = 1.0;  // deg C or % above target -> MEDIUM
const float DEVIATION_HIGH = 2.5;  // deg C or % above target -> HIGH

// Hysteresis band — prevents speed bouncing when deviation sits on a boundary.
// Fan only drops DOWN once deviation falls below (band - hysteresis).
const float TEMP_HYSTERESIS = 0.5;  // deg C
const float HUM_HYSTERESIS  = 2.0;  // %

// Minimum occupancy % to activate the fan at all
const int OCCUPANCY_THRESHOLD = 15;

// ==========================================
// 4. TASK SCHEDULING INTERVALS
// ==========================================
const unsigned long HVAC_INTERVAL       = 2500; // ms - DHT22 minimum period
// const unsigned long BREAK_BEAM_INTERVAL = 20;   // ms - future
// const unsigned long LOAD_CELL_INTERVAL  = 100;  // ms - future (HX711 @ 10Hz)

// ==========================================
// 5. GLOBAL OBJECTS & SHARED STATE
// ==========================================
DHT dht(DHTPIN, DHTTYPE);

// int simulatedOccupancy = 16; // Swap for real occupancy sensor later
// static int simulatedOccupancy = 0; // Start at 0% occupancy on boot

// Track last known speed for hysteresis and kickstart evaluation
int g_lastFanSpeed = SPEED_OFF;

// Scheduler timestamps - one per task
unsigned long g_lastHvacRead = 0;
// unsigned long g_lastBreakBeamRead = 0;  // future
// unsigned long g_lastLoadCellRead  = 0;  // future

// ==========================================
// 6. SERIAL COMMAND HANDLER
// ==========================================
// Allows live testing without re-flashing.
// Commands via Serial Monitor:
//   occ:50   -> Set occupancy to 50%  (changes active setpoint tier)
//   fan:255  -> Prime hysteresis state (simulate fan was previously at speed)
void handleSerialCommands() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd.startsWith("occ:")) {
      simulatedOccupancy = cmd.substring(4).toFloat();
      Serial.printf("[CMD] Occupancy set to %d%%\n", simulatedOccupancy);
    }
    else if (cmd.startsWith("fan:")) {
      int spd = cmd.substring(4).toInt();
      g_lastFanSpeed = spd;
      Serial.printf("[CMD] g_lastFanSpeed forced to %d\n", spd);
    }
    else {
      Serial.println("[CMD] Unknown. Try: occ:50  or  fan:255");
    }
  }
}

// ==========================================
// 7. FAN CONTROL
// ==========================================
// Non-blocking fan setter with kickstart pulse.
// The 150ms blocking delay only fires on an OFF -> ON transition,
// not on every call, so it does not meaningfully impact other tasks.
void setFanSpeed(int targetSpeed) {
  bool fanWasOff    = (g_lastFanSpeed == SPEED_OFF);
  bool fanTurningOn = (targetSpeed > SPEED_OFF);

  if (fanWasOff && fanTurningOn) {
    // Kick-start: momentarily blast full power to overcome motor stiction
    ledcWrite(PWMA_PIN, SPEED_HIGH);
    delay(KICKSTART_MS);
  }

  //ledcWrite(PWMA_PIN, targetSpeed);
  ledcWrite(PWM_CHANNEL, targetSpeed);
  g_lastFanSpeed = targetSpeed;
}

// ==========================================
// 8. SETPOINT RESOLVER
// ==========================================
// Returns the active comfort setpoint for the current occupancy level.
// Walks the table from highest tier downward — first match wins.
ComfortSetpoint getActiveSetpoint(int occupancy) {
  for (int i = NUM_SETPOINTS - 1; i >= 0; i--) {
    if (occupancy >= SETPOINTS[i].occupancyMin) {
      return SETPOINTS[i];
    }
  }
  return SETPOINTS[0]; // Fallback to lowest tier
}

// ==========================================
// 9. SETPOINT-BASED SPEED EVALUATOR
// ==========================================
// Replaces the old fixed-threshold evaluateTargetSpeed().
// Instead of asking "did we cross 28C?", we ask:
//   "how far are we above the comfort target for this occupancy level?"
// The fan speed is proportional to that deviation.
int evaluateTargetSpeed(float t, float h) {
  ComfortSetpoint sp = getActiveSetpoint(simulatedOccupancy);

  // Positive deviation = above target (too hot/humid) -> need more fan
  // Negative deviation = below target (comfortable)   -> minimum fan
  float tempDeviation = t - sp.targetTemp;
  float humDeviation  = h - sp.targetHum;

  // Apply hysteresis: only drop DOWN once deviation falls below
  // (band - hysteresis). Going UP always uses the standard band.
  float effectiveDeviationHigh = (g_lastFanSpeed == SPEED_HIGH)
    ? (DEVIATION_HIGH - TEMP_HYSTERESIS)
    : DEVIATION_HIGH;

  float effectiveDeviationMed = (g_lastFanSpeed >= SPEED_MED)
    ? (DEVIATION_MED - TEMP_HYSTERESIS)
    : DEVIATION_MED;

  // Either temp OR humidity deviation can escalate fan speed
  if (tempDeviation >= effectiveDeviationHigh ||
      humDeviation  >= effectiveDeviationHigh) return SPEED_HIGH;

  if (tempDeviation >= effectiveDeviationMed  ||
      humDeviation  >= effectiveDeviationMed)  return SPEED_MED;

  return SPEED_LOW; // Within or below target — maintain minimum circulation
}

// ==========================================
// 10. TASK FUNCTIONS
// ==========================================

// --- HVAC Task ---
// Called on its own interval. Reads DHT22, resolves active setpoint,
// evaluates deviation, and drives the fan accordingly.
void taskHvac() {
  // --- TEST OVERRIDE: Uncomment to test without hardware ---
  // float t = 26.0;
  // float h = 58.0;
  // --- END OVERRIDE ---

  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (isnan(t) || isnan(h)) {
    Serial.println("CRITICAL: Sensor read failure. Halting fan.");
    setFanSpeed(SPEED_OFF);
    return;
  }

  int    targetSpeed;
  String stateLabel;

  simulatedOccupancy = constrain(simulatedOccupancy, 0, 100);

  if (simulatedOccupancy < OCCUPANCY_THRESHOLD) {
    targetSpeed = SPEED_OFF;
    stateLabel  = "STANDBY (Empty Room)";
  } else {
    targetSpeed = evaluateTargetSpeed(t, h);
    switch (targetSpeed) {
      case SPEED_HIGH: stateLabel = "ACTIVE: HIGH";   break;
      case SPEED_MED:  stateLabel = "ACTIVE: MEDIUM"; break;
      default:         stateLabel = "ACTIVE: LOW";    break;
    }
  }

  setFanSpeed(targetSpeed);

  // Telemetry: shows live readings alongside the active setpoint targets
  // so you can see both where you ARE and where the system WANTS to be.
  ComfortSetpoint sp = getActiveSetpoint(simulatedOccupancy);
  // Serial.printf(
  //   "[HVAC]  Occ: %d%% (%s) | "
  //   "Temp: %.1fC (target %.1fC, dev %+.1f) | "
  //   "Hum: %.1f%% (target %.1f%%, dev %+.1f) | "
  //   "Fan: %d/255 -> %s\n",
  //   simulatedOccupancy, sp.label,
  //   t, sp.targetTemp, t - sp.targetTemp,
  //   h, sp.targetHum,  h - sp.targetHum,
  //   targetSpeed, stateLabel.c_str()
  // );
}

// --- FUTURE: Break Beam Task ---
// Uncomment and implement when sensor is added.
// Attach an interrupt to BREAK_BEAM_PIN for best reliability,
// or poll here at 20ms for a simpler first integration.
//
// volatile bool g_beamBroken = false;
// void IRAM_ATTR onBeamBreak() { g_beamBroken = true; }
//
// void taskBreakBeam() {
//   if (g_beamBroken) {
//     Serial.println("[BEAM] Beam interrupted - object detected.");
//     g_beamBroken = false;
//   }
// }

// --- FUTURE: Load Cell Task ---
// HX711 library provides its own is_ready() check.
// Scale.is_ready() returns true when DOUT goes LOW.
//
// void taskLoadCell() {
//   if (scale.is_ready()) {
//     long rawWeight = scale.read();
//     Serial.printf("[LOAD] Raw weight: %ld\n", rawWeight);
//   }
// }

// ==========================================
// 11. SETUP
// ==========================================
void HVAC_init() {
  Serial.println("System Boot: HVAC Prototype");

  dht.begin();

  pinMode(AIN1_PIN, OUTPUT);
  pinMode(AIN2_PIN, OUTPUT);

  digitalWrite(AIN1_PIN, HIGH);
  digitalWrite(AIN2_PIN, LOW);

  //ledcAttach(PWMA_PIN, PWM_FREQ, PWM_RESOLUTION);
  ledcSetup(PWM_CHANNEL, 25000, PWM_RESOLUTION); //Changed PWM_FREQ to 25000 cos I could hear it buzzing lmao
  ledcAttachPin(PWMA_PIN, PWM_CHANNEL);
  

  ledcWrite(PWM_CHANNEL, SPEED_OFF);
  // ledcWrite(PWMA_PIN, SPEED_OFF); // Safe state on boot

  // --- FUTURE SENSOR INIT ---
  // pinMode(BREAK_BEAM_PIN, INPUT_PULLUP);
  // attachInterrupt(digitalPinToInterrupt(BREAK_BEAM_PIN), onBeamBreak, FALLING);
  // scale.begin(LOAD_CELL_DOUT, LOAD_CELL_SCK);

  delay(2000); // DHT22 boot stabilization - acceptable here, runs once
  Serial.println("Sensors ready. Entering main loop.");

  // Print active setpoint table on boot so you know what targets are loaded
  Serial.println("--- Comfort Setpoints ---");
  for (int i = 0; i < NUM_SETPOINTS; i++) {
    Serial.printf("  Occ >= %d%%: Temp target %.1fC | Hum target %.1f%%  (%s)\n",
      SETPOINTS[i].occupancyMin,
      SETPOINTS[i].targetTemp,
      SETPOINTS[i].targetHum,
      SETPOINTS[i].label);
  }
  Serial.println("-------------------------");
}

// ==========================================
// 12. HVAC LOOP
// ==========================================
void HVAC_loop(){
    handleSerialCommands();

  unsigned long now = millis();

  // --- HVAC Task: runs every 2500ms ---
  if (now - g_lastHvacRead >= HVAC_INTERVAL) {
    g_lastHvacRead = now;
    taskHvac();
  }

  // --- FUTURE: Break Beam Task: runs every 20ms ---
  // if (now - g_lastBreakBeamRead >= BREAK_BEAM_INTERVAL) {
  //   g_lastBreakBeamRead = now;
  //   taskBreakBeam();
  // }

  // --- FUTURE: Load Cell Task: runs every 100ms ---
  // if (now - g_lastLoadCellRead >= LOAD_CELL_INTERVAL) {
  //   g_lastLoadCellRead = now;
  //   taskLoadCell();
  // }
}



void setOccupancy(int occ) {
    simulatedOccupancy = constrain(occ, 0, 100);
}
