#include <Arduino.h>

#define OUTER_PIN 17
#define INNER_PIN 5

unsigned long lastDetectedTime = 0;
bool timeoutTriggered = false;

// ================= SENSOR STATES =================
static int outerState = HIGH;
static int innerState = HIGH;

static int lastOuter = HIGH;
static int lastInner = HIGH;

// ================= OCCUPANCY DATA =================
static int count = 0;
static int entries = 0;
static int exits = 0;

// ================= STATE MACHINE =================
enum State {
    IDLE,
    OUTER_TRIGGERED,
    INNER_TRIGGERED
};

static State state = IDLE;

// ================= TIMERS =================
static unsigned long lastTriggerTime = 0;
static unsigned long lastCountTime = 0;

const unsigned long TIMEOUT  = 500; // max allowed time between sensors
const unsigned long COOLDOWN = 300; // ignore retriggers after count

// =================================================
// INIT
// =================================================
void IR_occupancy_init() {
    pinMode(OUTER_PIN, INPUT_PULLUP);
    pinMode(INNER_PIN, INPUT_PULLUP);
    lastDetectedTime = millis();
}

// =================================================
// UPDATE
// =================================================
void IR_occupancy_update() {

    unsigned long now = millis();

    // ---------- COOLDOWN ----------
    if (now - lastCountTime < COOLDOWN) {
        return;
    }

    // ---------- READ SENSORS ----------
    outerState = digitalRead(OUTER_PIN);
    innerState = digitalRead(INNER_PIN);

    // ---------- EDGE DETECTION ----------
    bool outerTriggered =
        (outerState == LOW && lastOuter == HIGH);

    bool innerTriggered =
        (innerState == LOW && lastInner == HIGH);

    // ---------- STATE MACHINE ----------
    switch (state) {

        // =================================
        case IDLE:

            if (outerTriggered) {
                state = OUTER_TRIGGERED;
                lastTriggerTime = now;
                Serial.println("Outer sensor triggered");
            }

            else if (innerTriggered) {
                state = INNER_TRIGGERED;
                lastTriggerTime = now;
                Serial.println("Inner sensor triggered");
            }

            break;

        // =================================
        case OUTER_TRIGGERED:

            // OUTER -> INNER = ENTRY
            if (innerTriggered) {

                count++;
                entries++;

                Serial.println("ENTRY DETECTED");

                lastCountTime = now;
                state = IDLE;
            }

            // timeout reset
            else if (now - lastTriggerTime > TIMEOUT) {
                state = IDLE;
            }

            break;

        // =================================
        case INNER_TRIGGERED:

            // INNER -> OUTER = EXIT
            if (outerTriggered) {

                count = max(0, count - 1);
                exits++;

                Serial.println("EXIT DETECTED");

                lastCountTime = now;
                state = IDLE;
            }

            // timeout reset
            else if (now - lastTriggerTime > TIMEOUT) {
                state = IDLE;
            }

            break;
    }

    // ---------- SAVE LAST STATES ----------
    lastOuter = outerState;
    lastInner = innerState;
}

// =================================================
// GETTERS
// =================================================
int get_count() {
    return count;
}

int get_entries() {
    return entries;
}

int get_exits() {
    return exits;
}

#include "HX711.h"
#include "soc/rtc.h"

// HX711 pins
const int LOADCELL_DOUT_PIN = 16;
const int LOADCELL_SCK_PIN  = 4;

const float CALIB_FACTOR1 = 2365.233;
const float CALIB_FACTOR2 = 2198.195;

HX711 scale;

// =====================================================
// Initialize load cell
// =====================================================
void loadcell_init() {

    Serial.println("Initializing scale...");

    scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);

    delay(500);

    if (!scale.is_ready()) {
        Serial.println("HX711 not found.");
        return;
    }

    Serial.println("HX711 ready.");

    scale.set_scale(2365.233f);

    scale.tare();

    Serial.println("Scale tared.");
}

// =====================================================
// Get weight
// =====================================================
float get_weight() {
    static float last_weight = 0.0; // Remember the last reading

    // If the scale is still calculating the next sample, 
    // just silently return the old weight!
    if (!scale.is_ready()) {
        return last_weight;
    }

    // Only grab a new reading when the HX711 is actually ready
    last_weight = scale.get_units(1); 
    return last_weight;
}

// ================= IR + LOAD LOGIC (COMBINED) ==================
float get_occupancy_percent(float weight, int count) {
    float weightPercent = (weight / 244.991344) * 100;
    float countPercent = (count / 4.0) * 100;

    weightPercent = constrain(weightPercent, 0, 100);
    countPercent = constrain(countPercent, 0, 100);

    return (weightPercent + countPercent) / 2.0;
}