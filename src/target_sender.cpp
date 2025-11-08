#include <Arduino.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <SPI.h>
#include <pgmspace.h>

// WiFi credentials - UPDATE THESE
const char *ssid = "Villa 1";
const char *password = "66669999";

// Web server on port 80
ESP8266WebServer server(80);

// SPI Configuration
// NodeMCU v2 pins: D5=GPIO14 (SCK), D7=GPIO13 (MOSI)
// Note: CS pin not needed for single master, but SPI library requires it
const int SPI_CS_PIN = 15; // D8 - dummy CS pin (not physically connected)

// Transmission settings
unsigned long transmissionRate =
    1000; // Default: 1000 Hz (1 byte per millisecond)
bool isTransmitting = false;
unsigned long lastTransmitTime = 0;
uint8_t testData = 0; // Test data counter

// Transmission mode
enum TransmissionMode {
  MODE_CONTINUOUS, // Continuous transmission with incrementing data
  MODE_ONEOFF      // One-off transmission with custom text
};
TransmissionMode transmissionMode = MODE_CONTINUOUS;

// One-off mode data
String oneOffText = "";
uint16_t oneOffIndex = 0;
bool oneOffComplete = false;

// HTML page for rate control
const char *htmlPage = R"HTML(
<!DOCTYPE html>
<html>
<head>
    <title>SPI Target Board Control</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial; margin: 20px; background: #f0f0f0; }
        .container { background: white; padding: 20px; border-radius: 10px; max-width: 500px; margin: 0 auto; }
        h1 { color: #333; }
        .input-group { margin: 15px 0; }
        label { display: block; margin-bottom: 5px; font-weight: bold; }
        input[type="number"] { width: 100%; padding: 10px; font-size: 16px; border: 1px solid #ddd; border-radius: 5px; }
        button { background: #4CAF50; color: white; padding: 12px 24px; border: none; border-radius: 5px; cursor: pointer; font-size: 16px; margin: 5px; }
        button:hover { background: #45a049; }
        button.stop { background: #f44336; }
        button.stop:hover { background: #da190b; }
        .status { margin-top: 20px; padding: 10px; background: #e7f3ff; border-radius: 5px; }
        .info { margin: 10px 0; color: #666; }
    </style>
</head>
<body>
    <div class="container">
        <h1>SPI Target Board Control</h1>
        
        <div class="input-group">
            <label for="mode">Transmission Mode:</label>
            <select id="mode" style="width: 100%; padding: 10px; font-size: 16px; border: 1px solid #ddd; border-radius: 5px;" onchange="onModeChange()">
                <option value="continuous">Continuous (Incrementing Data)</option>
                <option value="oneoff">One-Off (Custom Text)</option>
            </select>
        </div>
        
        <div class="input-group" id="textInputGroup" style="display:none;">
            <label for="textInput">Text to Send:</label>
            <input type="text" id="textInput" placeholder="Enter text to transmit..." style="width: 100%; padding: 10px; font-size: 16px; border: 1px solid #ddd; border-radius: 5px;">
            <div class="info">Text will be sent byte-by-byte via SPI</div>
        </div>
        
        <div class="input-group">
            <label for="rate">Transmission Rate (Hz):</label>
            <input type="number" id="rate" min="1" max="1000000" value="1000" step="1">
            <div class="info">Range: 1 Hz to 1,000,000 Hz</div>
        </div>
        
        <button onclick="setRate()">Set Rate</button>
        <button onclick="startTransmit()" id="startBtn">Start Transmission</button>
        <button onclick="stopTransmit()" class="stop" id="stopBtn" style="display:none;">Stop Transmission</button>
        
        <div class="status" id="status">
            <strong>Status:</strong> <span id="statusText">Ready</span><br>
            <strong>Current Rate:</strong> <span id="currentRate">1000</span> Hz<br>
            <strong>Transmitting:</strong> <span id="transmitting">No</span>
        </div>
    </div>
    
    <script>
        function onModeChange() {
            const mode = document.getElementById('mode').value;
            const textInputGroup = document.getElementById('textInputGroup');
            if (mode === 'oneoff') {
                textInputGroup.style.display = 'block';
            } else {
                textInputGroup.style.display = 'none';
            }
        }
        
        function setRate() {
            const rate = document.getElementById('rate').value;
            fetch('/setrate?rate=' + rate)
                .then(r => r.text())
                .then(data => {
                    document.getElementById('currentRate').textContent = rate;
                    document.getElementById('statusText').textContent = 'Rate set to ' + rate + ' Hz';
                });
        }
        
        function startTransmit() {
            const mode = document.getElementById('mode').value;
            const modeParam = mode === 'oneoff' ? '&mode=oneoff' : '&mode=continuous';
            const textParam = mode === 'oneoff' ? '&text=' + encodeURIComponent(document.getElementById('textInput').value) : '';
            
            fetch('/start' + modeParam + textParam)
                .then(r => r.text())
                .then(data => {
                    document.getElementById('transmitting').textContent = 'Yes';
                    document.getElementById('statusText').textContent = 'Transmitting...';
                    document.getElementById('startBtn').style.display = 'none';
                    document.getElementById('stopBtn').style.display = 'inline-block';
                });
        }
        
        function stopTransmit() {
            fetch('/stop')
                .then(r => r.text())
                .then(data => {
                    document.getElementById('transmitting').textContent = 'No';
                    document.getElementById('statusText').textContent = 'Stopped';
                    document.getElementById('startBtn').style.display = 'inline-block';
                    document.getElementById('stopBtn').style.display = 'none';
                });
        }
    </script>
</body>
</html>
)HTML";

void handleRoot() { server.send(200, "text/html", htmlPage); }

void handleSetRate() {
  if (server.hasArg("rate")) {
    transmissionRate = server.arg("rate").toInt();
    if (transmissionRate < 1)
      transmissionRate = 1;
    if (transmissionRate > 1000000)
      transmissionRate = 1000000;
    server.send(200, "text/plain",
                "Rate set to " + String(transmissionRate) + " Hz");
    Serial.println("Transmission rate set to: " + String(transmissionRate) +
                   " Hz");
  } else {
    server.send(400, "text/plain", "Missing rate parameter");
  }
}

void handleStart() {
  // Get mode parameter
  if (server.hasArg("mode")) {
    String mode = server.arg("mode");
    if (mode == "oneoff") {
      transmissionMode = MODE_ONEOFF;
      // Get text parameter
      if (server.hasArg("text")) {
        oneOffText = server.arg("text");
        oneOffIndex = 0;
        oneOffComplete = false;
        Serial.println("One-off mode: Text = \"" + oneOffText + "\"");
      } else {
        server.send(400, "text/plain",
                    "Missing text parameter for one-off mode");
        return;
      }
    } else {
      transmissionMode = MODE_CONTINUOUS;
      testData = 0; // Reset counter
      Serial.println("Continuous mode: Incrementing data");
    }
  }

  isTransmitting = true;
  server.send(200, "text/plain", "Transmission started");
  Serial.println("SPI transmission started");
}

void handleStop() {
  isTransmitting = false;
  oneOffComplete = false;
  oneOffIndex = 0;
  server.send(200, "text/plain", "Transmission stopped");
  Serial.println("SPI transmission stopped");
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n=== SPI Target Board (Sender) ===");

  // Initialize SPI
  SPI.begin();
  pinMode(SPI_CS_PIN, OUTPUT);
  digitalWrite(SPI_CS_PIN,
               HIGH); // CS high (inactive) - not used but required by library

  Serial.println("SPI initialized");
  Serial.println("Pins: D5 (GPIO14) = SCK, D7 (GPIO13) = MOSI");
  Serial.println(
      "Note: CS pin (D8) not physically connected - single master mode");

  // Connect to WiFi
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi connection failed!");
    Serial.println("Please check your SSID and password in the code.");
    return;
  }

  // Setup web server routes
  server.on("/", handleRoot);
  server.on("/setrate", handleSetRate);
  server.on("/start", handleStart);
  server.on("/stop", handleStop);

  server.begin();
  Serial.println("Web server started on http://" + WiFi.localIP().toString());
  Serial.println(
      "Open this URL in your browser to control the transmission rate");
}

void loop() {
  server.handleClient();

  // SPI transmission logic
  if (isTransmitting) {
    unsigned long currentTime = micros();
    unsigned long interval =
        1000000UL / transmissionRate; // Convert Hz to microseconds

    if (currentTime - lastTransmitTime >= interval) {
      // Use hardware SPI to send data
      // This will generate SCK and MOSI signals that the capture board can
      // monitor CS pin is set but not physically connected (single master mode)

      // Begin SPI transaction with appropriate settings
      SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
      digitalWrite(SPI_CS_PIN, LOW); // CS low (active) - required by library

      uint8_t dataToSend = 0;

      if (transmissionMode == MODE_CONTINUOUS) {
        // Continuous mode: send incrementing data
        dataToSend = testData;
        testData++; // Increment for next transmission
      } else if (transmissionMode == MODE_ONEOFF) {
        // One-off mode: send text byte by byte
        if (oneOffIndex < oneOffText.length()) {
          dataToSend = oneOffText.charAt(oneOffIndex);
          oneOffIndex++;

          // Check if we've sent all characters
          if (oneOffIndex >= oneOffText.length()) {
            oneOffComplete = true;
            isTransmitting = false; // Auto-stop after sending all text
            Serial.println("One-off transmission complete");
          }
        } else {
          // Already sent all text, stop transmission
          isTransmitting = false;
          digitalWrite(SPI_CS_PIN, HIGH);
          SPI.endTransaction();
          return;
        }
      }

      // Send data via SPI.transfer()
      // This generates SCK clock and sends data on MOSI
      SPI.transfer(dataToSend);

      digitalWrite(SPI_CS_PIN, HIGH); // CS high (inactive)
      SPI.endTransaction();

      lastTransmitTime = currentTime;
    }
  }
}
