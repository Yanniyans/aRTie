#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "DHT.h"
#include "hvac.h"
#include "occupancy.h"
#include "door.h"
#include "HX711.h"
#include "soc/rtc.h"

// --- WIFI SETTINGS ---
const char* ssid = "PEKACHU24";
const char* password = "24KM@gicChunky"; 
String googleScriptURL = "https://script.google.com/macros/s/AKfycbx3rqRuCq4xEUGWZ9a590nl35CO9T70OQjm_V1bStLdLcP-P4HHj8wKvC0Yjjx4AuAl/exec";
//https://script.google.com/macros/s/AKfycbx3rqRuCq4xEUGWZ9a590nl35CO9T70OQjm_V1bStLdLcP-P4HHj8wKvC0Yjjx4AuAl/exec?action=update&car1=99&car2=0
int count = 0;
int occupancy_percent = 0;
float weight = 0;

// define train states
enum state_train {
  IN_TRANSIT,
  DOORS_OPENING,
  DOORS_CLOSING,
};

state_train trainState = IN_TRANSIT;

// --- CLOUD PUSH FUNCTION WITH RETRY LOOP ---
void pushOccupancyToCloud(int car1, int car2) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    // Format the URL for an update action
    String writeURL = googleScriptURL + "?action=update&car1=" + String(car1) + "&car2=" + String(car2);

    Serial.printf("\n[Cloud] Sending Occupancy -> Car 1: %d%% | Car 2: %d%%\n", car1, car2);

    int httpResponseCode = -1;
    int attempts = 0;
    const int MAX_ATTEMPTS = 3;

    // Retry loop in case of traffic jams
    while (httpResponseCode <= 0 && attempts < MAX_ATTEMPTS) {
      if (attempts > 0) {
        Serial.printf("[Cloud] Retry attempt %d of %d...\n", attempts + 1, MAX_ATTEMPTS);
        delay(2000); // Breathe for 2 seconds before retrying
      }
      
      http.begin(client, writeURL);
      httpResponseCode = http.GET();
      attempts++;
    }

    if (httpResponseCode > 0) {
      Serial.printf("[Cloud] Success! HTTP Code: %d\n", httpResponseCode);
    } else {
      Serial.printf("[Cloud] Error after %d attempts: %d\n", MAX_ATTEMPTS, httpResponseCode);
    }
    http.end();
  } else {
    Serial.println("[Cloud] Error: Not connected to Wi-Fi. Data not sent.");
  }
}

// ==========================================================================
//                Main Logic
// ==========================================================================
void setup() {
  Serial.begin(115200);

  // 1. Connect to Wi-Fi first so we know aRTie is online
  Serial.print("\n[Train ESP] Connecting to Wi-Fi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ Wi-Fi Connected!");

  // 2. Initialize all hardware sensors
  IR_occupancy_init();
  HVAC_init();
  loadcell_init();
  Door_btn_Init();
}

void loop() {

  switch (trainState) {
    case IN_TRANSIT:
      Door_btn_Update();  

      if(button_pressed) {
        trainState = DOORS_OPENING;
        Serial.println("\n>>> Door button pressed. Simulating doors opening...");
        delay(500); // Debounce delay so it doesn't instantly skip to closing
      }

      HVAC_loop();
      break;

    case DOORS_OPENING:
      IR_occupancy_update();
      count = get_count();
      weight = get_weight();
      occupancy_percent = (int)get_occupancy_percent(weight, count);
      setOccupancy(occupancy_percent);

      // ---> THE FIX: Run the HVAC loop while doors are open! <---
      HVAC_loop(); 

      // NON-BLOCKING PRINT (Prints every 500ms instead of spamming)
      static unsigned long lastPrint = 0;
      if (millis() - lastPrint > 500) {
        Serial.printf("Occupancy: %d%% | Count: %d | Weight: %.2f\n", occupancy_percent, count, weight);
        lastPrint = millis();
      }

      Door_btn_Update();  
      if(button_pressed) {
        trainState = DOORS_CLOSING;
        Serial.println("\n>>> Door button pressed again. Simulating doors closing...");
        delay(500); // Debounce delay
      }
      break;

    case DOORS_CLOSING:
      Serial.println("[State] DOORS CLOSING -> Snapping final occupancy count!");
      
      // Push the data to the cloud. We pass 0 for Car 2 since this is a 1-car prototype.
      pushOccupancyToCloud(occupancy_percent, 0);

      trainState = IN_TRANSIT;
      Serial.println("[State] Train is back IN TRANSIT.\n");
      break;
  }
}
