#include <esp_now.h>
#include <WiFi.h>
#include <HardwareSerial.h>
#include <DFRobotDFPlayerMini.h>

// ====================== CONFIG ======================
const char* WIFI_SSID = "Netenza_FDC1D0";
const char* WIFI_PASS = "aA12345!";

uint8_t mainEspMac[] = { 0x88, 0x57, 0x21, 0x6A, 0x03, 0x48 };

// Display & Audio Settings
const int DEFAULT_BRIGHTNESS = 200;
const int PWM_FREQ = 5000;
const int PWM_BITS = 10;
const unsigned long DIGIT_DISPLAY_TIME_US = 2000;
const unsigned long DEAD_TIME_US = 100;

// ====================== PINS & OBJECTS ======================
HardwareSerial dfPlayerSerial(2);
DFRobotDFPlayerMini player;

const int DIGIT_PINS[3] = { 13, 14, 27 };
const int SEGMENT_PINS[7] = { 18, 5, 4, 32, 33, 25, 26 };

// 7-segment patterns for digits 0-9
const bool SEGMENT_PATTERNS[10][7] = {
  {1,1,1,1,1,1,0}, {0,1,1,0,0,0,0}, {1,1,0,1,1,0,1}, {1,1,1,1,0,0,1},
  {0,1,1,0,0,1,1}, {1,0,1,1,0,1,1}, {1,0,1,1,1,1,1}, {1,1,1,0,0,0,0},
  {1,1,1,1,1,1,1}, {1,1,1,1,0,1,1}
};

// Idle animation pattern (only segment G - middle line)
const bool IDLE_PATTERN[7] = {0,0,0,0,0,0,1}; // Only segment G

// ====================== THREAD-SAFE VARIABLES ======================
volatile int currentNumber = -1;
volatile int displayBrightness = DEFAULT_BRIGHTNESS;
volatile bool hasReceivedData = false;
SemaphoreHandle_t xDataMutex = NULL;

// Simplified Audio Variables (No State Machine)
volatile int audioQueue = -1; // -1 means nothing to play
SemaphoreHandle_t xAudioMutex = NULL;

// ====================== LOGGING HELPER ======================
void logInfo(const char* module, const char* message) {
  Serial.printf("[%lu ms] [%-10s] %s\n", millis(), module, message);
}

void logData(const char* module, const char* format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  logInfo(module, buffer);
}

// ====================== DISPLAY HELPER FUNCTIONS ======================
void displayDigit(int digit, int position) {
  // Turn off all digits
  for (int i = 0; i < 3; i++) {
    digitalWrite(DIGIT_PINS[i], LOW);
  }

  // Set segments for current digit
  for (int seg = 0; seg < 7; seg++) {
    uint8_t intensity = SEGMENT_PATTERNS[digit][seg] ? displayBrightness : 0;
    ledcWrite(SEGMENT_PINS[seg], intensity);
  }

  // Dead time
  delayMicroseconds(DEAD_TIME_US);

  // Turn on digit
  digitalWrite(DIGIT_PINS[position], HIGH);
  delayMicroseconds(DIGIT_DISPLAY_TIME_US);
  digitalWrite(DIGIT_PINS[position], LOW);
}

void displayIdleAnimation() {
  // Display segment G (middle line) on all three digits
  for (int i = 0; i < 3; i++) {
    digitalWrite(DIGIT_PINS[i], LOW);
  }

  for (int seg = 0; seg < 7; seg++) {
    uint8_t intensity = IDLE_PATTERN[seg] ? displayBrightness : 0;
    ledcWrite(SEGMENT_PINS[seg], intensity);
  }

  delayMicroseconds(DEAD_TIME_US);

  // Turn on all digits
  for (int i = 0; i < 3; i++) {
    digitalWrite(DIGIT_PINS[i], HIGH);
  }
  delayMicroseconds(DIGIT_DISPLAY_TIME_US);

  for (int i = 0; i < 3; i++) {
    digitalWrite(DIGIT_PINS[i], LOW);
  }
}

void clearDisplay() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(DIGIT_PINS[i], LOW);
  }
  for (int seg = 0; seg < 7; seg++) {
    ledcWrite(SEGMENT_PINS[seg], 0);
  }
}

void refreshDisplay(int number, bool showIdle) {
  if (showIdle) {
    displayIdleAnimation();
    return;
  }

  if (number < 0) {
    clearDisplay();
    return;
  }

  // Extract digits
  int digits[3];
  digits[0] = number / 100;
  digits[1] = (number / 10) % 10;
  digits[2] = number % 10;

  // Determine which digits to display
  bool showDigit[3] = {
    number >= 100,
    number >= 10,
    true
  };

  // Display each digit
  for (int i = 0; i < 3; i++) {
    if (showDigit[i]) {
      displayDigit(digits[i], i);
    }
  }
}

// ====================== SIMPLIFIED AUDIO HANDLER ======================
void checkAndPlayAudio() {
  // Check if we can access audio variables
  if (xSemaphoreTake(xAudioMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return;
  }

  // If there is a valid number in the queue, play it
  if (audioQueue >= 0 && audioQueue <= 999) {
    logData("AUDIO", "▶ DFPlayer playing track: %d", audioQueue);
    player.play(audioQueue);
    
    // Reset queue immediately after command sent
    audioQueue = -1;
  }

  xSemaphoreGive(xAudioMutex);
}

// ====================== ESP-NOW CALLBACK ======================
void onDataRecv(const esp_now_recv_info *info, const uint8_t *data, int len) {
  if (len != sizeof(int)) {
    logData("ESP-NOW", "❌ Wrong data size: %d bytes", len);
    return;
  }

  int tempVal = 0;
  memcpy(&tempVal, data, sizeof(int));

  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           info->src_addr[0], info->src_addr[1], info->src_addr[2],
           info->src_addr[3], info->src_addr[4], info->src_addr[5]);
  logData("ESP-NOW", "📡 Data received from %s", macStr);

  if (tempVal < 0 || tempVal > 999) {
    logData("ESP-NOW", "❌ Number out of range: %d", tempVal);
    return;
  }

  // Update display
  if (xSemaphoreTake(xDataMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    if (!hasReceivedData) {
      logInfo("DISPLAY", "✓ First data received!");
      hasReceivedData = true;
    }
    currentNumber = tempVal;
    logData("DISPLAY", "🔢 Number updated: %d", tempVal);
    xSemaphoreGive(xDataMutex);
  }

  // Queue audio (Simple assignment)
  if (xSemaphoreTake(xAudioMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    audioQueue = tempVal;   
    logData("AUDIO", "🎵 Audio queued: %d", tempVal);
    xSemaphoreGive(xAudioMutex);
  }
}

// ====================== DISPLAY TASK (CORE 1) ======================
void displayTask(void * parameter) {
  logInfo("TASK", "✓ Display Task started (Core 1)");

  while (true) {
    // Part 1: Digital display
    int displayNum = -1;
    bool hasData = false;

    if (xSemaphoreTake(xDataMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      displayNum = currentNumber;
      hasData = hasReceivedData;
      xSemaphoreGive(xDataMutex);
    }

    refreshDisplay(displayNum, !hasData);

    // Part 2: Check Audio Queue
    checkAndPlayAudio();

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// ====================== INITIALIZATION ======================
void initializePins() {
  logInfo("PINS", "🔧 Starting pin configuration...");

  // Digit control pins
  for (int i = 0; i < 3; i++) {
    pinMode(DIGIT_PINS[i], OUTPUT);
    digitalWrite(DIGIT_PINS[i], LOW);
  }
  logInfo("PINS", "✓ Digit pins configured");

  // Segment PWM pins
  for (int i = 0; i < 7; i++) {
    ledcAttach(SEGMENT_PINS[i], PWM_FREQ, PWM_BITS);
    ledcWrite(SEGMENT_PINS[i], 0);
  }
  logInfo("PINS", "✓ Segment (PWM) pins configured");
}

void initializeDFPlayer() {
  logInfo("DFPlayer", "🔊 Starting DFPlayer connection...");

  dfPlayerSerial.begin(9600, SERIAL_8N1, 16, 17);

  int maxRetries = 5;
  for (int i = 0; i < maxRetries; i++) {
    if (player.begin(dfPlayerSerial)) {
      delay(1000);
      player.volume(5);
      logInfo("DFPlayer", "✓ DFPlayer successfully identified!");
      logData("DFPlayer", "  Volume set: 5/30");
      return;
    }
    logData("DFPlayer", "⏳ Retry (%d/%d)", i + 1, maxRetries);
    delay(500);
  }

  logInfo("DFPlayer", "❌ Error: DFPlayer not found (check connections)");
}

void initializeWiFiAndESPNow() {
  logInfo("WiFi", "📶 Starting WiFi connection...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    logData("WiFi", "✓ WiFi connected");
    logData("WiFi", "  SSID: %s", WIFI_SSID);
    logData("WiFi", "  IP: %s", WiFi.localIP().toString().c_str());
  } else {
    logInfo("WiFi", "⚠ WiFi not connected (will continue trying)");
  }

  logInfo("ESP-NOW", "📡 Starting ESP-NOW initialization...");
  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onDataRecv);
    logInfo("ESP-NOW", "✓ ESP-NOW started");
  } else {
    logInfo("ESP-NOW", "❌ Error: ESP-NOW failed to start");
  }
}

// ====================== SETUP ======================
void setup() {
  Serial.begin(115200);
  delay(10000);

  logInfo("SETUP", "🚀 Startup process started");

  // Create mutexes
  xDataMutex = xSemaphoreCreateMutex();
  xAudioMutex = xSemaphoreCreateMutex();

  if (xDataMutex == NULL || xAudioMutex == NULL) {
    logInfo("SETUP", "❌ Error: Mutex creation failed");
    while (1);
  }
  logInfo("SETUP", "✓ Mutexes created");

  // Initialize hardware
  initializePins();
  delay(100);
  initializeDFPlayer();
  delay(100);
  initializeWiFiAndESPNow();
  delay(100);

  // Create display task on core 1
  BaseType_t taskCreated = xTaskCreatePinnedToCore(
    displayTask,
    "DisplayTask",
    4096,
    NULL,
    configMAX_PRIORITIES - 1,
    NULL,
    1
  );

  if (taskCreated != pdPASS) {
    logInfo("SETUP", "❌ Error: Display Task creation failed");
  } else {
    logInfo("SETUP", "✓ Display Task running on Core 1");
  }

  logInfo("SETUP", "✅ Setup completed!\n");
}

// ====================== MAIN LOOP ======================
void loop() {
  delay(100);

  // Monitor system health every 10 seconds
  if (millis() % 10000 == 0) {
    logData("HEALTH", "🏥 Free Heap: %u bytes | Core0 Stack: %u",
    esp_get_free_heap_size(),
    uxTaskGetStackHighWaterMark(NULL));
  }
}