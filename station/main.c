#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// --- WIFI & CLOUD SETTINGS ---
const char* ssid = "PEKACHU24";
const char* password = "24KM@gicChunky";
// PASTE YOUR EXACT NEW DEPLOYMENT URL HERE:
String googleScriptURL = "https://script.google.com/macros/s/AKfycbxeCoXVrCublrLcypYnCB23ckDqf4MEIhhIFRA0nJunfhlfExV4virHldxXyRgZLJy5/exec"; 

// --- HARDWARE PINS ---
const int platformDisplayLed = 13;
const int DEPARTURE_PIN = 32;

// --- STATION TRACKING ---
unsigned long last_departure_time = 0;
const char* stations[] = {"Recto", "Legarda", "Pureza", "V. Mapa", "J. Ruiz", "Gilmore", "Betty Go-Belmonte", "Cubao", "Anonas", "Katipunan", "Santolan", "Marikina", "Antipolo"};
int current_station_index = 9; // Starts at Katipunan
int last_train_state = HIGH;

// --- THREADING & SHARED DATA ---
portMUX_TYPE jsonMux = portMUX_INITIALIZER_UNLOCKED;
String globalJsonData = "{}";
int bestOcc = 100;

WebServer server(80);

// =========================================================================
// HTML DASHBOARD PAYLOAD
// =========================================================================
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>aRTie | Dashboard</title>
  <style>
    body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: #f4f4f9; color: #333; margin: 0; padding: 15px; }
    h2, h3, h4 { margin: 0 0 10px 0; color: #1a1a1a; }
    .card { background: white; border-radius: 12px; padding: 20px; margin-bottom: 15px; box-shadow: 0 4px 6px rgba(0,0,0,0.05); position: relative; border: 2px solid #e0e0e0; transition: border-color 0.2s;}
    .nav-card { cursor: pointer; }
    .nav-card:active { border-color: #ff9800; }
    .highlight-text { font-size: 32px; font-weight: bold; text-align: center; margin: 10px 0; color: #ff9800; border-top: 2px solid #333; padding-top: 10px;}
    .arrow { position: absolute; bottom: 15px; right: 20px; font-size: 24px; font-weight: bold; color: #ff9800; }
    .back-btn { background: #333; color: white; border: none; padding: 10px 15px; border-radius: 8px; margin-bottom: 15px; cursor: pointer; font-size: 16px; }
    .fare-grid { display: flex; justify-content: space-between; align-items: center; }
    .fare-col { width: 48%; }
    .vertical-line { border-left: 2px solid #333; height: 60px; }
    select, input { width: 100%; padding: 8px; margin-bottom: 5px; border: 1px solid #ccc; border-radius: 4px; font-size: 14px;}
    table { width: 100%; border-collapse: collapse; margin-top: 10px; }
    th, td { padding: 12px; text-align: left; border-bottom: 1px solid #ddd; }
    th { border-bottom: 2px solid #333; }
    .view { display: none; }
    .active-view { display: block; }
  </style>
</head>
<body>

  <div id="view-home" class="view active-view">
    <div class="card nav-card" onclick="switchView('view-cars')">
      <h3>least occupied car:</h3>
      <div class="highlight-text" id="main-occ">Loading...</div>
      <div class="arrow">&rarr;</div>
    </div>

    <div class="card">
      <div style="display: flex; gap: 10px; margin-bottom: 15px;">
        <select id="ticket-type" onchange="calculateFare()" style="flex: 1;">
          <option value="beep">Stored Value</option>
          <option value="sj">Single Journey</option>
        </select>
        <select id="discount-type" onchange="calculateFare()" style="flex: 1;">
          <option value="reg">Regular Fare</option>
          <option value="disc">50% Discount</option>
        </select>
      </div>
      <div class="fare-grid">
        <div class="fare-col">
          <label>Starting:</label>
          <select id="start-station" onchange="calculateFare()">
            <option value="0">Recto</option><option value="1">Legarda</option>
            <option value="2">Pureza</option><option value="3">V. Mapa</option>
            <option value="4">J. Ruiz</option><option value="5">Gilmore</option>
            <option value="6">Betty Go</option><option value="7">Cubao</option>
            <option value="8">Anonas</option><option value="9" selected>Katipunan</option>
            <option value="10">Santolan</option><option value="11">Marikina</option>
            <option value="12">Antipolo</option>
          </select>
          <label>Ending:</label>
          <select id="end-station" onchange="calculateFare()">
            <option value="0">Recto</option><option value="1">Legarda</option>
            <option value="2">Pureza</option><option value="3">V. Mapa</option>
            <option value="4">J. Ruiz</option><option value="5">Gilmore</option>
            <option value="6">Betty Go</option><option value="7" selected>Cubao</option>
            <option value="8">Anonas</option><option value="9">Katipunan</option>
            <option value="10">Santolan</option><option value="11">Marikina</option>
            <option value="12">Antipolo</option>
          </select>
        </div>
        <div class="vertical-line"></div>
        <div class="fare-col" style="text-align: center;">
          <h3>Price:</h3>
          <h2 id="fare-price" style="color: #4caf50;">--</h2>
        </div>
      </div>
    </div>

    <div class="card nav-card" onclick="switchView('view-history')">
      <h4 id="main-prev-label">Left &lt;Prev. Station&gt;:</h4>
      <div class="highlight-text" style="font-size: 28px; color: #333;" id="prev-station-time">--:-- --</div>
      <div class="arrow">&rarr;</div>
    </div>
  </div>

  <div id="view-cars" class="view">
    <button class="back-btn" onclick="switchView('view-home')">&larr; Back</button>
    <div class="card">
      <h3>least occupied car:</h3>
      <div class="highlight-text" id="detail-occ">--</div>
    </div>
    <div class="card">
      <table id="cars-table">
        <tr><th>Other cars</th><th>capacity</th></tr>
        </table>
    </div>
  </div>

  <div id="view-history" class="view">
    <button class="back-btn" onclick="switchView('view-home')">&larr; Back</button>
    <div class="card">
      <h4 id="detail-prev-label">Left &lt;Prev. Station&gt;:</h4>
      <div style="font-size: 24px; font-weight: bold; text-align: center; margin-top:10px;" id="hist-station-time">--:-- --</div>
    </div>
    <div class="card">
      <table id="history-table">
        <tr><th>station name</th><th>time</th></tr>
        </table>
    </div>
  </div>

  <script>
    const fareMatrix = [
      [13, 15, 16, 18, 19, 21, 22, 23, 25, 26, 28, 31, 33],
      [15, 13, 15, 17, 18, 19, 21, 22, 24, 25, 27, 29, 32],
      [16, 15, 13, 15, 16, 18, 19, 20, 22, 23, 26, 28, 30],
      [18, 17, 15, 13, 15, 16, 17, 19, 20, 22, 24, 26, 29],
      [19, 18, 16, 15, 13, 14, 16, 17, 19, 20, 22, 24, 27],
      [21, 19, 18, 16, 14, 13, 15, 16, 18, 19, 21, 23, 26],
      [22, 21, 19, 17, 16, 15, 13, 15, 16, 18, 20, 22, 25],
      [23, 22, 20, 19, 17, 16, 15, 13, 15, 16, 19, 21, 23],
      [25, 24, 22, 20, 19, 18, 16, 15, 13, 14, 17, 19, 22],
      [26, 25, 23, 22, 20, 19, 18, 16, 14, 13, 16, 18, 21],
      [28, 27, 26, 24, 22, 21, 20, 19, 17, 16, 13, 15, 18],
      [31, 29, 28, 26, 24, 23, 22, 21, 19, 18, 15, 13, 16],
      [33, 32, 30, 29, 27, 26, 25, 23, 22, 21, 18, 16, 13]
    ];
    function switchView(viewId) {
      document.querySelectorAll('.view').forEach(el => el.classList.remove('active-view'));
      document.getElementById(viewId).classList.add('active-view');
    }

    function calculateFare() {
      let start = parseInt(document.getElementById('start-station').value);
      let end = parseInt(document.getElementById('end-station').value);
      let isSingleJourney = document.getElementById('ticket-type').value === "sj";
      let isDiscounted = document.getElementById('discount-type').value === "disc";
      let priceDisplay = document.getElementById('fare-price');
      if (start === end) {
        priceDisplay.innerText = "0";
      } else {
        let svReg = fareMatrix[start][end];
        let finalFare = svReg;
        if (isSingleJourney) finalFare = Math.ceil(svReg / 5) * 5;
        if (isDiscounted) finalFare = isSingleJourney ? Math.round(finalFare / 2) : (finalFare / 2).toFixed(2);
        priceDisplay.innerHTML = "&#8369;" + finalFare;
      }
    }
    calculateFare();

    setInterval(function() {
      fetch('/api/data')
        .then(response => response.json())
        .then(data => {
          if(!data.error) {
            let bestText = data.bestCarName + " : " + data.bestCarOcc + "% full";
            document.getElementById('main-occ').innerText = bestText;
            document.getElementById('detail-occ').innerText = bestText;
            
            document.getElementById('main-prev-label').innerText = "Left " + data.currentStation + ":";
            document.getElementById('detail-prev-label').innerText = "Left " + data.currentStation + ":";
            
            document.getElementById('prev-station-time').innerText = data.currentTime;
            document.getElementById('hist-station-time').innerText = data.currentTime;
            
            let mainOcc = document.getElementById('main-occ');
            if(data.bestCarOcc < 50) mainOcc.style.color = "#4caf50";
            else if(data.bestCarOcc < 80) mainOcc.style.color = "#ffeb3b";
            else mainOcc.style.color = "#f44336";

            let carsHtml = "<tr><th>Other cars</th><th>capacity</th></tr>";
            for(let i=0; i < data.allCars.length; i++) {
              let carNum = i + 1;
              let isBest = (data.allCars[i] === data.bestCarOcc) ? "style='color: #4caf50; font-weight:bold;'" : "";
              let headTail = (i===0) ? " (Head)" : ((i === data.allCars.length - 1) ? " (Tail)" : "");
              carsHtml += "<tr><td>Car " + carNum + headTail + "</td><td " + isBest + ">" + data.allCars[i] + "%</td></tr>";
            }
            document.getElementById('cars-table').innerHTML = carsHtml;
            
            let histHtml = "<tr><th>station name</th><th>time</th></tr>";
            for(let i=0; i < data.history.length; i++) {
              histHtml += "<tr><td>" + data.history[i].station + "</td><td>" + data.history[i].time + "</td></tr>";
            }
            document.getElementById('history-table').innerHTML = histHtml;
          }
        })
        .catch(err => console.error('Fetch error:', err));
    }, 1000); 
  </script>
</body>
</html>
)rawliteral";


// =========================================================================
// CLOUD DEPARTURE FUNCTION (WITH RETRY LOOP & URL FIX)
// =========================================================================
void send_departure_to_cloud() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(15000);

    // ---> THE CRITICAL FIX: The "?" is now correctly placed before station <---
    String writeURL = googleScriptURL + "?station=" + String(stations[current_station_index]) + "&car1=0&car2=0";

    Serial.println("\n[Cloud] Trying to send: " + writeURL);

    int httpResponseCode = -1;
    int attempts = 0;
    const int MAX_ATTEMPTS = 3;

    // Retry loop in case of traffic jams
    while (httpResponseCode <= 0 && attempts < MAX_ATTEMPTS) {
      if (attempts > 0) {
        Serial.printf("[Cloud] Retry attempt %d of %d...\n", attempts + 1, MAX_ATTEMPTS);
        delay(2000);
      }
      http.begin(client, writeURL);
      httpResponseCode = http.GET();
      attempts++;
    }

    if (httpResponseCode > 0) {
      Serial.printf("[Cloud] Departure Logged! HTTP Code: %d\n", httpResponseCode);
      String payload = http.getString();
      Serial.println("Google said: " + payload);
    } else {
      Serial.printf("[Cloud] Error updating departure after %d attempts: %d\n", MAX_ATTEMPTS, httpResponseCode);
    }
    http.end();
  } else {
    Serial.println("[Cloud] Error: Wi-Fi disconnected. Data not sent.");
  }
}


// =========================================================================
// CORE 1: BACKGROUND TASK (FETCHES LIVE DATA FROM CLOUD)
// =========================================================================
void googlePollTask(void *pvParameters) {
  while (true) {
    if (WiFi.status() == WL_CONNECTED) {
      WiFiClientSecure client;
      client.setInsecure();
      HTTPClient http;
      http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

      String readURL = googleScriptURL + "?action=read";
      http.begin(client, readURL);
      int httpResponseCode = http.GET();

      if (httpResponseCode > 0) {
        String payload = http.getString();

        // Lock memory while updating the shared JSON
        portENTER_CRITICAL(&jsonMux);
        globalJsonData = payload;
        portEXIT_CRITICAL(&jsonMux);

        // Quick parse to check best car occupancy for the Platform LED
        int occIndex = payload.indexOf("\"bestCarOcc\":");
        if (occIndex != -1) {
          int startQuote = payload.indexOf(":", occIndex) + 1;
          int endComma = payload.indexOf(",", startQuote);
          if (endComma == -1) endComma = payload.indexOf("}", startQuote);
          String occStr = payload.substring(startQuote, endComma);
          bestOcc = occStr.toInt();

          if (bestOcc < 50) {
            digitalWrite(platformDisplayLed, HIGH); // Spacious car arriving!
          } else {
            digitalWrite(platformDisplayLed, LOW);
          }
        }
        Serial.println("[Core 1] Cloud Synced JSON payload!");
      }
      http.end();
    }
    vTaskDelay(5000 / portTICK_PERIOD_MS); // Pull data every 5 seconds
  }
}


// =========================================================================
// CORE 0: WEB SERVER ROUTES
// =========================================================================
void handleRoot() {
  server.send(200, "text/html", index_html);
}

void handleData() {
  portENTER_CRITICAL(&jsonMux);
  String dataToSend = globalJsonData;
  portEXIT_CRITICAL(&jsonMux);
  server.send(200, "application/json", dataToSend);
}


// =========================================================================
// SETUP & MAIN LOOP
// =========================================================================
void setup() {
  Serial.begin(115200);
  
  pinMode(platformDisplayLed, OUTPUT);
  pinMode(DEPARTURE_PIN, INPUT_PULLUP);
  digitalWrite(platformDisplayLed, LOW);

  // 1. Start the Local Hotspot
  Serial.println("\n[Station ESP] Starting aRTie Hotspot...");
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("aRTie_Katipunan", "artiethegoat");
  Serial.print("Hotspot IP: ");
  Serial.println(WiFi.softAPIP());

  // 2. Connect to the Internet
  WiFi.begin(ssid, password);
  Serial.print("Connecting to Internet");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ Internet Connected!");

  // 3. Start the Web Server (NOTE: /api/data to match your HTML fetch!)
  server.on("/", handleRoot);
  server.on("/api/data", handleData); 
  server.begin();
  Serial.println("✅ Web Server Started on Core 0!");

  // 4. Launch the Background Cloud Polling Task on Core 1
  xTaskCreatePinnedToCore(
    googlePollTask,   // Function to run
    "GooglePollTask", // Name of task
    10000,            // Stack size (bytes)
    NULL,             // Parameter
    1,                // Task priority
    NULL,             // Task handle
    1                 // Pin to Core 1
  );
}

void loop() {
  // Always handle web server clients first
  server.handleClient();

  // Check the physical IR Track Sensor
  int current_train_state = digitalRead(DEPARTURE_PIN);

  // If the sensor transitions from LOW (blocked) to HIGH (clear) = Train Departs!
  if (last_train_state == LOW && current_train_state == HIGH) {
    if (millis() - last_departure_time > 10000) { // 10-second cooldown
      
      Serial.printf("\n[Station] Train just left %s\n", stations[current_station_index]);

      // Fire the Cloud Function
      send_departure_to_cloud();

      // Cycle the index to the next station
      current_station_index++;
      if (current_station_index >= 13) {
        current_station_index = 0; // Loop back around to the beginning
      }

      last_departure_time = millis();
    }
  }
  last_train_state = current_train_state;
}
