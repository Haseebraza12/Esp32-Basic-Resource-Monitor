// ESP32 Resource Monitor — STA with AP Fallback, Background Load + Dashboard
// ═══════════════════════════════════════════════════════════════════════════════

// FreeRTOS run-time stats (for vTaskGetRunTimeStats)
#define configGENERATE_RUN_TIME_STATS    1
#define portGET_RUN_TIME_COUNTER_VALUE() esp_timer_get_time()
#define portCONFIG_RUN_TIME_COUNTER_VALUE_TYPE uint64_t

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h> // Make sure you have this library installed
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h> // For sin, cos, sqrt in CPU load tasks

// — Your home Wi-Fi (STA) credentials —
// IMPORTANT: Replace with your actual Wi-Fi SSID and Password
const char* STA_SSID = "Haseeb muneeb"; // Your Wi-Fi network name
const char* STA_PWD  = "03234342638";    // Your Wi-Fi password

// — Web server —
AsyncWebServer server(80);

// — CPU usage tracking —
// Volatile to ensure the compiler doesn't optimize access, as these are modified by tasks
volatile float cpu0Usage = 0.0;
volatile float cpu1Usage = 0.0;

// — Background load toggle —
// Used to control if the CPU load tasks should run
volatile bool loadEnabled = true;

// — Compute CPU usage via run-time stats —
// Static to maintain state across calls, initialized to 0
static uint64_t lastIdle0 = 0, lastIdle1 = 0;
static uint64_t lastTot0 = 0, lastTot1 = 0;

void calculateCPU() {
  char buffer[512]; // Buffer to hold the run-time stats
  // Get FreeRTOS run-time stats into the buffer
  vTaskGetRunTimeStats(buffer);

  uint64_t currentIdle0 = 0, currentIdle1 = 0;
  uint64_t currentTot0 = 0, currentTot1 = 0;

  // strtok modifies the buffer, so make a copy if you need the original for something else
  char* line = strtok(buffer, "\n");
  while (line) {
    char name[32];      // Task name
    uint32_t runTime;   // Task run time (ticks)
    uint32_t percentage; // Percentage of CPU time (not directly used for calculation here)

    // Parse each line of the run-time stats
    if (sscanf(line, "%31s %u %u", name, &runTime, &percentage) == 3) {
      // Check for IDLE tasks for each core
      if (strstr(name, "IDLE0")) {
        currentIdle0 = runTime;
      } else if (strstr(name, "IDLE1")) { // Using else if for distinct cores
        currentIdle1 = runTime;
      }
      // Sum total run time for each core (assuming tasks are pinned correctly)
      currentTot0 += runTime;
      currentTot1 += runTime;
    }
    line = strtok(nullptr, "\n"); // Get next line
  }

  // Calculate CPU usage for Core 0
  if (currentTot0 > lastTot0) { // Avoid division by zero and ensure progress
    uint64_t dIdle0 = currentIdle0 - lastIdle0;
    uint64_t dTot0  = currentTot0  - lastTot0;
    // CPU Usage = (Total Time - Idle Time) / Total Time * 100
    cpu0Usage = dTot0 > 0 ? (float)(dTot0 - dIdle0) / dTot0 * 100.0f : 0;
  }

  // Calculate CPU usage for Core 1
  if (currentTot1 > lastTot1) { // Avoid division by zero and ensure progress
    uint64_t dIdle1 = currentIdle1 - lastIdle1;
    uint64_t dTot1  = currentTot1  - lastTot1;
    // CPU Usage = (Total Time - Idle Time) / Total Time * 100
    cpu1Usage = dTot1 > 0 ? (float)(dTot1 - dIdle1) / dTot1 * 100.0f : 0;
  }

  // Update last values for the next calculation
  lastIdle0 = currentIdle0;
  lastIdle1 = currentIdle1;
  lastTot0  = currentTot0;
  lastTot1  = currentTot1;
}

// — Simple CPU-burn tasks to generate load —
// These tasks will continuously perform calculations to simulate CPU load.
// They are designed to run indefinitely until 'loadEnabled' is false.

void cpuLoad0(void*) {
  TickType_t wakeTime = xTaskGetTickCount(); // Initialize wake time for vTaskDelayUntil
  while (loadEnabled) {
    volatile double accumulator = 0; // Use volatile to prevent compiler optimization
    // Perform some floating-point operations to consume CPU cycles
    for (uint32_t i = 0; i < 50000; ++i) {
      accumulator += sin(i * 0.001) * cos(i * 0.001) + sqrt(i + 1);
    }
    // Delay for a short period to allow other tasks to run and avoid 100% busy loop
    vTaskDelayUntil(&wakeTime, pdMS_TO_TICKS(10)); // Delay for 10 milliseconds
  }
  vTaskDelete(NULL); // Delete the task when loadEnabled is false
}

void cpuLoad1(void*) {
  TickType_t wakeTime = xTaskGetTickCount(); // Initialize wake time for vTaskDelayUntil
  while (loadEnabled) {
    volatile uint64_t fib = 1, prev = 0, tmp; // Fibonacci sequence calculation
    // Perform some integer operations to consume CPU cycles
    for (uint32_t i = 0; i < 30000; ++i) {
      tmp = fib;
      fib += prev;
      prev = tmp;
      if (fib > 1000000) fib = 1; // Reset to prevent overflow for uint64_t
    }
    // Delay for a short period to allow other tasks to run
    vTaskDelayUntil(&wakeTime, pdMS_TO_TICKS(15)); // Delay for 15 milliseconds
  }
  vTaskDelete(NULL); // Delete the task when loadEnabled is false
}

// — Dashboard HTML (inline for brevity) —
// This HTML includes the CSS for styling and JavaScript for fetching and updating metrics.
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32 Resource Monitor</title>
  <style>
    body {
      margin: 0;
      padding: 20px;
      font-family: 'Inter', sans-serif; /* Using Inter for consistency */
      background: linear-gradient(135deg, #667eea, #764ba2);
      color: #fff;
      display: flex;
      flex-direction: column;
      align-items: center;
      min-height: 100vh;
      box-sizing: border-box; /* Include padding in element's total width and height */
    }
    h1 {
      text-align: center;
      margin-bottom: 20px;
      text-shadow: 2px 2px 4px rgba(0,0,0,.3);
      font-size: 2.5rem; /* Larger heading */
    }
    .container {
      width: 100%;
      max-width: 600px; /* Max width for better readability on large screens */
    }
    .card {
      background: rgba(255,255,255,.9);
      color: #333;
      border-radius: 12px;
      padding: 20px;
      margin-bottom: 20px;
      box-shadow: 0 4px 8px rgba(0,0,0,.2); /* Subtle shadow */
      transition: transform 0.2s ease-in-out; /* Smooth hover effect */
    }
    .card:hover {
      transform: translateY(-5px); /* Lift card on hover */
    }
    .label {
      font-weight: 600;
      margin-bottom: 5px;
      display: block;
      color: #555; /* Slightly darker label color */
    }
    .value {
      font-size: 1.8rem; /* Larger value font size */
      margin-bottom: 10px;
      font-weight: bold;
      color: #007bff; /* Highlight value with a distinct color */
    }
    .bar {
      width: 100%;
      height: 15px; /* Taller bar */
      background: #e0e0e0; /* Lighter background for bar */
      border-radius: 8px; /* More rounded corners */
      overflow: hidden;
      margin-top: 10px;
    }
    .fill {
      height: 100%;
      width: 0%;
      /* Significantly reduced transition duration for more real-time feel */
      background: linear-gradient(to right, #ff6b6b, #ee4c7d); /* Gradient for fill */
      transition: width .1s ease-out; /* Smooth transition for width */
      border-radius: 8px;
    }
    small {
      font-size: 0.9rem;
      color: #666;
      display: block; /* Ensure it takes full width */
      margin-top: 5px;
    }
    /* Responsive adjustments */
    @media (max-width: 768px) {
      body {
        padding: 15px;
      }
      h1 {
        font-size: 2rem;
      }
      .value {
        font-size: 1.5rem;
      }
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>🚀 ESP32 Resource Monitor</h1>

    <div class="card">
      <span class="label">Heap:</span>
      <span id="heapVal" class="value">0 / 0 KB</span>
      <div class="bar"><div id="heapBar" class="fill"></div></div>
    </div>

    <div class="card">
      <span class="label">Wi-Fi RSSI:</span>
      <span id="rssi" class="value">0 dBm</span>
    </div>

    <div class="card">
      <span class="label">CPU Usage:</span>
      <span id="cpuVal" class="value">0%</span>
      <div class="bar"><div id="cpuBar" class="fill"></div></div>
      <small>Core 0: <span id="cpu0">0</span>%&nbsp;Core 1: <span id="cpu1">0</span>%</small>
    </div>
  </div>

<script>
async function fetchMetrics() {
  try {
    // Fetch data from the /metrics endpoint
    let response = await fetch('/metrics');
    // Check if the response is OK (status code 200)
    if (!response.ok) {
      console.error('Failed to fetch metrics:', response.statusText);
      return;
    }
    let data = await response.json(); // Parse the JSON response

    // Update Heap
    const heapValElement = document.getElementById('heapVal');
    const heapBarElement = document.getElementById('heapBar');
    const heap_free = data.heap_free;
    const heap_total = data.heap_total;
    const heapFreeKB = (heap_free / 1024) | 0;
    const heapTotalKB = (heap_total / 1024) | 0;
    heapValElement.innerText = heapFreeKB + ' / ' + heapTotalKB + ' KB';
    const heapPercentage = (heap_total > 0) ? (100 * (heap_total - heap_free) / heap_total).toFixed(1) : 0;
    heapBarElement.style.width = heapPercentage + '%';
    // Change bar color based on heap usage
    if (heapPercentage > 80) {
      heapBarElement.style.background = 'linear-gradient(to right, #dc3545, #c82333)'; // Red for high usage
    } else if (heapPercentage > 50) {
      heapBarElement.style.background = 'linear-gradient(to right, #ffc107, #e0a800)'; // Orange for medium usage
    } else {
      heapBarElement.style.background = 'linear-gradient(to right, #28a745, #218838)'; // Green for low usage
    }


    // Update Wi-Fi RSSI
    document.getElementById('rssi').innerText = data.rssi + ' dBm';

    // Update CPU Usage
    const cpuValElement = document.getElementById('cpuVal');
    const cpuBarElement = document.getElementById('cpuBar');
    const cpu0Usage = parseFloat(data.cpu0);
    const cpu1Usage = parseFloat(data.cpu1);
    const avgCpuUsage = ((cpu0Usage + cpu1Usage) / 2).toFixed(1);
    cpuValElement.innerText = avgCpuUsage + '%';
    cpuBarElement.style.width = avgCpuUsage + '%';
    // Change bar color based on CPU usage
    if (avgCpuUsage > 80) {
      cpuBarElement.style.background = 'linear-gradient(to right, #dc3545, #c82333)'; // Red for high usage
    } else if (avgCpuUsage > 50) {
      cpuBarElement.style.background = 'linear-gradient(to right, #ffc107, #e0a800)'; // Orange for medium usage
    } else {
      cpuBarElement.style.background = 'linear-gradient(to right, #28a745, #218838)'; // Green for low usage
    }


    document.getElementById('cpu0').innerText = cpu0Usage.toFixed(1);
    document.getElementById('cpu1').innerText = cpu1Usage.toFixed(1);

  } catch (error) {
    console.error('Error fetching or parsing metrics:', error);
    // Optionally update UI to show error state
    document.getElementById('heapVal').innerText = 'Error';
    document.getElementById('rssi').innerText = 'Error';
    document.getElementById('cpuVal').innerText = 'Error';
  }
}

// Fetch metrics more frequently for a real-time feel (every 200 milliseconds)
setInterval(fetchMetrics, 200);

// Fetch metrics immediately on page load
fetchMetrics();
</script>
</body>
</html>
)rawliteral";

void setup() {
  Serial.begin(115200); // Initialize serial communication for debugging
  delay(500); // Small delay to allow serial to initialize
  Serial.println("\n🚀 ESP32 Resource Monitor Starting...");

  // 1) Try to join your Wi-Fi as STA (Station)
  WiFi.mode(WIFI_STA); // Set Wi-Fi to Station mode
  WiFi.begin(STA_SSID, STA_PWD); // Connect to the specified Wi-Fi network
  Serial.printf("🌐 STA: Connecting to %s", STA_SSID);
  uint8_t connectAttempts = 0;
  // Wait for Wi-Fi connection, with a timeout
  while (WiFi.status() != WL_CONNECTED && connectAttempts < 30) {
    delay(500); // Wait 500ms
    Serial.print("."); // Print a dot for each attempt
    connectAttempts++;
  }

  // Check if network is connected, and if not, try AP mode
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ STA connected!");
  } else {
    Serial.println("\n⚠️ STA failed; starting AP “ESP32-Monitor”");
    WiFi.mode(WIFI_AP); // Set Wi-Fi to Access Point mode
    WiFi.softAP("ESP32-Monitor");
    delay(500); // Give some time for AP to start
  }

  // 3) Print the final IP address (either STA or AP)
  IPAddress ip = (WiFi.status() == WL_CONNECTED)
    ? WiFi.localIP()    // If connected as STA, get local IP
    : WiFi.softAPIP(); // If in AP mode, get SoftAP IP

  Serial.printf("📡 Dashboard available at: http://%s\n", ip.toString().c_str());

  // 4) Launch background load tasks
  // These tasks simulate CPU activity on both cores.
  // Pinned to core 0 (second to last argument 0) with priority 2
  xTaskCreatePinnedToCore(cpuLoad0, "CPU0_Load", 4096, NULL, 2, NULL, 0);
  // Pinned to core 1 (second to last argument 1) with priority 2
  xTaskCreatePinnedToCore(cpuLoad1, "CPU1_Load", 4096, NULL, 2, NULL, 1);

  // 5) Start web server
  // Handle root URL ("/") to serve the HTML dashboard
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request){
    request->send_P(200, "text/html", INDEX_HTML); // Send the HTML content
  });

  // Handle "/metrics" URL to serve JSON data
  server.on("/metrics", HTTP_GET, [](AsyncWebServerRequest* request){
    calculateCPU(); // Calculate current CPU usage before sending metrics

    // Get various system metrics
    size_t freeHeap  = heap_caps_get_free_size(MALLOC_CAP_DEFAULT); // Free heap memory
    size_t totalHeap = heap_caps_get_total_size(MALLOC_CAP_DEFAULT); // Total heap memory
    
    int wifiRSSI = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0; // Wi-Fi signal strength

    // Create a JSON document to hold the metrics
    // StaticJsonDocument is preferred for fixed-size JSON to avoid dynamic memory allocation issues
    StaticJsonDocument<256> doc; // Allocate a buffer for JSON (adjust size if more data is added)
    doc["heap_free"] = freeHeap;
    doc["heap_total"]= totalHeap;
    doc["rssi"]      = wifiRSSI;
    doc["cpu0"]      = cpu0Usage; // CPU usage for core 0
    doc["cpu1"]      = cpu1Usage; // CPU usage for core 1

    String output; // String to hold the serialized JSON
    serializeJson(doc, output); // Serialize JSON document to a String

    // Send the JSON response with content type "application/json"
    request->send(200, "application/json", output);
  });

  server.begin(); // Start the web server
  Serial.println("Web server started.");
}

void loop() {
  // In ESP-IDF/FreeRTOS, most operations are handled by tasks or async events.
  // The loop() function can be left empty or used for very light, non-blocking tasks.
  // The web server and CPU load tasks run in the background.
  delay(100); // Small delay to yield to other tasks, though generally not strictly needed with FreeRTOS
}