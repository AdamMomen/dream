#include <Arduino.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <WebSocketsServer.h>

// WiFi credentials - UPDATE THESE
const char *ssid = "Villa 1";
const char *password = "66669999";

// Web server on port 80, WebSocket server on port 81
ESP8266WebServer server(80);
WebSocketsServer webSocket(81);

// SPI Pin Configuration
// NodeMCU v2 pins: D5=GPIO14 (SCK), D6=GPIO12 (MISO)
const int SPI_SCK_PIN = 14;  // D5 - Clock pin (interrupt on this)
const int SPI_MISO_PIN = 12; // D6 - Data pin (read on clock edge)

// Circular Buffer Configuration (1KB = 1024 bytes)
#define BUFFER_SIZE 1024

// Data structure for captured samples
struct Sample {
  uint8_t data;            // MISO data bit (0 or 1)
  unsigned long timestamp; // Microsecond timestamp
};

// Circular buffer
volatile Sample buffer[BUFFER_SIZE];
volatile uint16_t bufferHead = 0; // Write pointer
volatile uint16_t bufferTail = 0; // Read pointer
volatile bool bufferOverflow = false;

// Interrupt handling variables
volatile unsigned long lastClockTime = 0;
volatile unsigned long lastSampleTime = 0;
volatile uint32_t sampleCount = 0;

// HTML page for frontend
const char *htmlPage = R"HTML(
<!DOCTYPE html>
<html>
<head>
    <title>SPI Logic Analyzer - Capture Board</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { 
            font-family: 'Courier New', monospace; 
            background: #1e1e1e; 
            color: #d4d4d4; 
            padding: 20px;
        }
        .container { 
            max-width: 1400px; 
            margin: 0 auto; 
        }
        h1 { 
            color: #4ec9b0; 
            margin-bottom: 20px; 
            text-align: center;
        }
        .status-bar {
            background: #252526;
            padding: 15px;
            border-radius: 5px;
            margin-bottom: 20px;
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
            gap: 15px;
        }
        .status-item {
            background: #2d2d30;
            padding: 10px;
            border-radius: 3px;
        }
        .status-label {
            color: #858585;
            font-size: 12px;
            text-transform: uppercase;
        }
        .status-value {
            color: #4ec9b0;
            font-size: 18px;
            font-weight: bold;
            margin-top: 5px;
        }
        .status-value.error { color: #f48771; }
        .status-value.warning { color: #dcdcaa; }
        .hex-display {
            background: #252526;
            padding: 15px;
            border-radius: 5px;
            margin-bottom: 20px;
            max-height: 400px;
            overflow-y: auto;
        }
        .hex-line {
            font-family: 'Courier New', monospace;
            font-size: 14px;
            line-height: 1.6;
            padding: 2px 0;
            border-bottom: 1px solid #3e3e42;
        }
        .hex-line:hover {
            background: #2d2d30;
        }
        .hex-address {
            color: #569cd6;
            display: inline-block;
            width: 80px;
        }
        .hex-data {
            color: #ce9178;
            display: inline-block;
            width: 400px;
            word-spacing: 8px;
        }
        .hex-ascii {
            color: #858585;
            display: inline-block;
            margin-left: 20px;
        }
        .connection-status {
            position: fixed;
            top: 10px;
            right: 10px;
            padding: 10px 20px;
            border-radius: 5px;
            font-weight: bold;
        }
        .connected { background: #4ec9b0; color: #1e1e1e; }
        .disconnected { background: #f48771; color: #1e1e1e; }
        .controls {
            background: #252526;
            padding: 15px;
            border-radius: 5px;
            margin-bottom: 20px;
        }
        button {
            background: #0e639c;
            color: white;
            border: none;
            padding: 10px 20px;
            border-radius: 3px;
            cursor: pointer;
            font-size: 14px;
            margin-right: 10px;
        }
        button:hover { background: #1177bb; }
        button:active { background: #0a4f75; }
        .info {
            background: #2d2d30;
            padding: 15px;
            border-radius: 5px;
            margin-top: 20px;
            color: #858585;
            font-size: 12px;
        }
    </style>
</head>
<body>
    <div class="connection-status disconnected" id="connectionStatus">Disconnected</div>
    <div class="container">
        <h1>SPI Logic Analyzer - Capture Board</h1>
        
        <div class="status-bar">
            <div class="status-item">
                <div class="status-label">Baud Rate</div>
                <div class="status-value" id="baudRate">0 Hz</div>
            </div>
            <div class="status-item">
                <div class="status-label">Sample Count</div>
                <div class="status-value" id="sampleCount">0</div>
            </div>
            <div class="status-item">
                <div class="status-label">Buffer Usage</div>
                <div class="status-value" id="bufferUsage">0%</div>
            </div>
            <div class="status-item">
                <div class="status-label">Samples Available</div>
                <div class="status-value" id="samplesAvailable">0</div>
            </div>
            <div class="status-item">
                <div class="status-label">Buffer Overflow</div>
                <div class="status-value" id="overflow">No</div>
            </div>
        </div>
        
        <div class="controls">
            <button onclick="clearDisplay()">Clear Display</button>
            <button onclick="toggleAutoScroll()" id="autoScrollBtn">Auto Scroll: ON</button>
        </div>
        
        <div class="hex-display" id="hexDisplay">
            <div style="color: #858585; text-align: center; padding: 20px;">
                Waiting for SPI data... Connect the target board and start transmission.
            </div>
        </div>
        
        <!-- <div class="info"> <strong>Instructions:</strong><br> 1. Connect target board (sender) to capture board via SPI (D5→D5, D7→D6, GND→GND)<br> 2. Open target board web interface and start transmission<br> 3. Captured data will appear here in real-time<br> 4. Data is displayed in hexadecimal format with ASCII representation </div> -->
    </div>
    
    <script>
        let ws = null;
        let autoScroll = true;
        let byteBuffer = [];
        let currentByte = 0;
        let bitPosition = 0;
        let address = 0;
        
        function connectWebSocket() {
            const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
            const wsUrl = protocol + '//' + window.location.hostname + ':81';
            
            ws = new WebSocket(wsUrl);
            
            ws.onopen = function() {
                console.log('WebSocket connected');
                document.getElementById('connectionStatus').textContent = 'Connected';
                document.getElementById('connectionStatus').className = 'connection-status connected';
            };
            
            ws.onclose = function() {
                console.log('WebSocket disconnected');
                document.getElementById('connectionStatus').textContent = 'Disconnected';
                document.getElementById('connectionStatus').className = 'connection-status disconnected';
                // Reconnect after 2 seconds
                setTimeout(connectWebSocket, 2000);
            };
            
            ws.onerror = function(error) {
                console.error('WebSocket error:', error);
            };
            
            ws.onmessage = function(event) {
                try {
                    const data = JSON.parse(event.data);
                    updateDisplay(data);
                } catch (e) {
                    console.error('Error parsing JSON:', e);
                }
            };
        }
        
        function updateDisplay(data) {
            // Update status bar
            document.getElementById('baudRate').textContent = formatNumber(data.baudRate) + ' Hz';
            document.getElementById('sampleCount').textContent = formatNumber(data.sampleCount);
            
            const bufferUsage = ((data.samplesAvailable / data.bufferSize) * 100).toFixed(1);
            document.getElementById('bufferUsage').textContent = bufferUsage + '%';
            document.getElementById('samplesAvailable').textContent = formatNumber(data.samplesAvailable);
            
            const overflowEl = document.getElementById('overflow');
            overflowEl.textContent = data.overflow ? 'Yes' : 'No';
            overflowEl.className = data.overflow ? 'status-value error' : 'status-value';
            
            // Process samples and build bytes
            if (data.samples && data.samples.length > 0) {
                const hexDisplay = document.getElementById('hexDisplay');
                
                // Clear "waiting" message if present
                if (hexDisplay.children.length === 1 && hexDisplay.children[0].textContent.includes('Waiting')) {
                    hexDisplay.innerHTML = '';
                }
                
                data.samples.forEach(sample => {
                    // Build byte from bits (MSB first, typical for SPI)
                    currentByte = (currentByte << 1) | sample.data;
                    bitPosition++;
                    
                    if (bitPosition >= 8) {
                        // Complete byte received
                        byteBuffer.push(currentByte);
                        currentByte = 0;
                        bitPosition = 0;
                        
                        // Display when we have 16 bytes (one line)
                        if (byteBuffer.length >= 16) {
                            displayHexLine(byteBuffer);
                            byteBuffer = [];
                        }
                    }
                });
                
                // Display remaining bytes if any
                if (byteBuffer.length > 0 && byteBuffer.length < 16) {
                    // Wait for more bytes or display partial line
                }
                
                // Auto scroll
                if (autoScroll) {
                    hexDisplay.scrollTop = hexDisplay.scrollHeight;
                }
            }
        }
        
        function displayHexLine(bytes) {
            const hexDisplay = document.getElementById('hexDisplay');
            const line = document.createElement('div');
            line.className = 'hex-line';
            
            // Address
            const addressSpan = document.createElement('span');
            addressSpan.className = 'hex-address';
            addressSpan.textContent = '0x' + padHex(address, 4);
            line.appendChild(addressSpan);
            
            // Hex data
            const dataSpan = document.createElement('span');
            dataSpan.className = 'hex-data';
            dataSpan.textContent = bytes.map(b => padHex(b, 2)).join(' ');
            line.appendChild(dataSpan);
            
            // ASCII representation
            const asciiSpan = document.createElement('span');
            asciiSpan.className = 'hex-ascii';
            asciiSpan.textContent = bytes.map(b => {
                return (b >= 32 && b <= 126) ? String.fromCharCode(b) : '.';
            }).join('');
            line.appendChild(asciiSpan);
            
            hexDisplay.appendChild(line);
            address += bytes.length;
            
            // Limit display to last 1000 lines to prevent memory issues
            while (hexDisplay.children.length > 1000) {
                hexDisplay.removeChild(hexDisplay.firstChild);
            }
        }
        
        function padHex(num, width) {
            return num.toString(16).toUpperCase().padStart(width, '0');
        }
        
        function formatNumber(num) {
            return num.toString().replace(/\B(?=(\d{3})+(?!\d))/g, ',');
        }
        
        function clearDisplay() {
            document.getElementById('hexDisplay').innerHTML = '';
            byteBuffer = [];
            currentByte = 0;
            bitPosition = 0;
            address = 0;
        }
        
        function toggleAutoScroll() {
            autoScroll = !autoScroll;
            document.getElementById('autoScrollBtn').textContent = 'Auto Scroll: ' + (autoScroll ? 'ON' : 'OFF');
        }
        
        // Connect on page load
        connectWebSocket();
    </script>
</body>
</html>
)HTML";

void handleRoot() { server.send(200, "text/html", htmlPage); }

// IRAM_ATTR ensures interrupt handler runs from IRAM (fast)
void IRAM_ATTR onClockEdge() {
  unsigned long currentTime = micros();

  // Read MISO pin state (data bit)
  uint8_t dataBit = digitalRead(SPI_MISO_PIN);

  // Calculate time since last clock edge (for baud rate calculation)
  if (lastClockTime > 0) {
    // timeDelta could be used for baud rate calculation if needed
    // unsigned long timeDelta = currentTime - lastClockTime;
  }
  lastClockTime = currentTime;

  // Store sample in circular buffer
  uint16_t nextHead = (bufferHead + 1) % BUFFER_SIZE;

  // Check for buffer overflow
  if (nextHead == bufferTail) {
    bufferOverflow = true;
    // Move tail forward (overwrite oldest data)
    bufferTail = (bufferTail + 1) % BUFFER_SIZE;
  }

  // Store sample
  buffer[bufferHead].data = dataBit;
  buffer[bufferHead].timestamp = currentTime;
  bufferHead = nextHead;

  sampleCount++;
  lastSampleTime = currentTime;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n=== SPI Capture Board (Receiver) ===");

  // Configure SPI pins
  pinMode(SPI_SCK_PIN, INPUT_PULLUP);  // SCK with pull-up
  pinMode(SPI_MISO_PIN, INPUT_PULLUP); // MISO with pull-up

  Serial.println("SPI pins configured:");
  Serial.println("  D5 (GPIO14) = SCK (Clock)");
  Serial.println("  D6 (GPIO12) = MISO (Data)");

  // Attach interrupt on SCK pin - trigger on both rising and falling edges
  // This captures data on both clock edges for maximum sampling rate
  attachInterrupt(digitalPinToInterrupt(SPI_SCK_PIN), onClockEdge, CHANGE);

  Serial.println("SPI capture interrupt configured");
  Serial.println("Capturing on both clock edges (CHANGE mode)");

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

  // Setup HTTP server (for frontend)
  server.on("/", handleRoot);
  server.begin();

  // Setup WebSocket server
  webSocket.begin();
  webSocket.onEvent(
      [](uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
        switch (type) {
        case WStype_DISCONNECTED:
          Serial.printf("Client [%u] disconnected\n", num);
          break;
        case WStype_CONNECTED:
          Serial.printf("Client [%u] connected from %s\n", num,
                        webSocket.remoteIP(num).toString().c_str());
          // Send initial status
          {
            String statusMsg = "{\"status\":\"connected\",\"bufferSize\":" +
                               String(BUFFER_SIZE) + "}";
            webSocket.sendTXT(num, statusMsg);
          }
          break;
        case WStype_TEXT:
          // Handle incoming messages if needed
          Serial.printf("Client [%u] sent: %s\n", num, payload);
          break;
        default:
          break;
        }
      });

  Serial.println("WebSocket server started on port 81");
  Serial.println("HTTP server started on port 80");
  Serial.println("Ready to capture SPI data!");
}

void loop() {
  server.handleClient();
  webSocket.loop();

  // Stream buffer data to all connected WebSocket clients
  static unsigned long lastStreamTime = 0;
  unsigned long currentTime = millis();

  // Stream data every 100ms (10 times per second)
  if (currentTime - lastStreamTime >= 100) {
    lastStreamTime = currentTime;

    // Calculate buffer status
    uint16_t samplesAvailable = 0;
    if (bufferHead >= bufferTail) {
      samplesAvailable = bufferHead - bufferTail;
    } else {
      samplesAvailable = BUFFER_SIZE - bufferTail + bufferHead;
    }

    // Calculate baud rate from timing (if we have samples)
    float baudRate = 0;
    if (sampleCount > 1 && lastClockTime > 0) {
      // Estimate baud rate from sample count and time
      unsigned long totalTime =
          lastSampleTime -
          (lastSampleTime - (sampleCount * 1000)); // Rough estimate
      if (totalTime > 0) {
        baudRate =
            (float)sampleCount / (totalTime / 1000000.0); // Samples per second
      }
    }

    // Create JSON payload with buffer data
    String json = "{";
    json += "\"samples\":[";

    // Send up to 100 samples at a time to avoid overwhelming the client
    uint16_t samplesToSend = min(samplesAvailable, (uint16_t)100);
    bool first = true;

    for (uint16_t i = 0; i < samplesToSend; i++) {
      uint16_t idx = (bufferTail + i) % BUFFER_SIZE;

      if (!first)
        json += ",";
      first = false;

      json += "{";
      json += "\"data\":" + String(buffer[idx].data) + ",";
      json += "\"timestamp\":" + String(buffer[idx].timestamp);
      json += "}";
    }

    json += "],";
    json += "\"bufferHead\":" + String(bufferHead) + ",";
    json += "\"bufferTail\":" + String(bufferTail) + ",";
    json += "\"samplesAvailable\":" + String(samplesAvailable) + ",";
    json += "\"bufferSize\":" + String(BUFFER_SIZE) + ",";
    json += "\"sampleCount\":" + String(sampleCount) + ",";
    json += "\"overflow\":" + String(bufferOverflow ? "true" : "false") + ",";
    json += "\"baudRate\":" + String(baudRate);
    json += "}";

    // Send to all connected clients
    webSocket.broadcastTXT(json.c_str(), json.length());

    // Update tail pointer (mark samples as sent)
    if (samplesToSend > 0) {
      bufferTail = (bufferTail + samplesToSend) % BUFFER_SIZE;
    }
  }
}
