#include <esp_now.h>
#include <WiFi.h>
#include <HardwareSerial.h>
#include <DFRobotDFPlayerMini.h>

// ====================== CONFIG ======================

// WiFi credentials – MUST be the same network as the main ESP
const char* WIFI_SSID = "Netenza_FDC1D0";
const char* WIFI_PASS = "aA12345!";

// Main ESP32 STA MAC address (sender)
// Set this to WiFi.macAddress() printed by the main ESP
uint8_t mainEspMac[] = { 0x00, 0x4B, 0x12, 0x3A, 0x76, 0xC8 };

// 7‑segment brightness (0..1023 for 10‑bit)
const int brightness = 90;

// ====================== DFPLAYER ======================
HardwareSerial mySerial(2); // RX2 / TX2
DFRobotDFPlayerMini player;

// ====================== 7 SEGMENT SETUP ======================
const int DIGIT_1 = 13;
const int DIGIT_2 = 14;
const int DIGIT_3 = 27;

const int SEG_A = 26;
const int SEG_B = 25;
const int SEG_C = 33;
const int SEG_D = 32;
const int SEG_E = 4;
const int SEG_F = 5;
const int SEG_G = 18;

const int digitPins[]   = { DIGIT_1, DIGIT_2, DIGIT_3 };
const int segmentPins[] = { SEG_G, SEG_F, SEG_E, SEG_D, SEG_C, SEG_B, SEG_A };

const bool segmentPatterns[10][7] = {
  {1,1,1,1,1,1,0}, // 0
  {0,1,1,0,0,0,0}, // 1
  {1,1,0,1,1,0,1}, // 2
  {1,1,1,1,0,0,1}, // 3
  {0,1,1,0,0,1,1}, // 4
  {1,0,1,1,0,1,1}, // 5
  {1,0,1,1,1,1,1}, // 6
  {1,1,1,0,0,0,0}, // 7
  {1,1,1,1,1,1,1}, // 8
  {1,1,1,1,0,1,1}  // 9
};

int currentNumber = 0;
int newNumber     = -1;
bool shouldPlay   = false;

// ====================== ESP-NOW CALLBACK ======================
void onDataRecv(const esp_now_recv_info *info, const uint8_t *data, int len) {
  if (len == sizeof(int)) {
    memcpy(&newNumber, data, sizeof(int));
    shouldPlay = true;
    Serial.printf("Received number via ESP-NOW: %d\n", newNumber);
  } else {
    Serial.printf("ESP-NOW: unexpected data length %d\n", len);
  }
}

// ====================== DISPLAY ======================
void displayNumberOn7Seg(int number) {
  int d[3] = { number / 100, (number / 10) % 10, number % 10 };

  bool showDigit[3] = {
    number >= 100,
    number >= 10,
    true
  };

  for (int i = 0; i < 3; i++) {
    // Turn all digits off
    for (int j = 0; j < 3; j++) {
      digitalWrite(digitPins[j], LOW);
    }

    if (showDigit[i]) {
      // Set segments for this digit
      for (int s = 0; s < 7; s++) {
        int pin = segmentPins[s];
        if (segmentPatterns[d[i]][s]) {
          ledcWrite(pin, brightness);
        } else {
          ledcWrite(pin, 0);
        }
      }
      // Enable current digit
      digitalWrite(digitPins[i], HIGH);
    } else {
      // Blank segments
      for (int s = 0; s < 7; s++) {
        int pin = segmentPins[s];
        ledcWrite(pin, 0);
      }
    }

    delayMicroseconds(600);
  }
}

// ====================== SETUP ======================
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("=== DISPLAY ESP START ===");

  // DFPLAYER
  mySerial.begin(9600, SERIAL_8N1, 16, 17);
  if (!player.begin(mySerial)) {
    Serial.println("DFPlayer NOT FOUND!");
  } else {
    player.volume(25);
    Serial.println("DFPlayer ready.");
  }

  // DIGITS
  for (int i = 0; i < 3; i++) {
    pinMode(digitPins[i], OUTPUT);
    digitalWrite(digitPins[i], LOW);
  }

  // SEGMENTS — LEDC PWM (ESP32 Core 3.x: channel = pin)
  for (int i = 0; i < 7; i++) {
    ledcAttach(segmentPins[i], 2000 /*Hz*/, 10 /*bits*/);
  }

  // WiFi + channel sync with main ESP
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(200);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi connected, STA MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("WiFi channel: ");
  Serial.println(WiFi.channel());

  // ESP-NOW init
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW Init Failed");
    return;
  }
  Serial.println("ESP-NOW Init OK");

  // Register recv callback
  esp_now_register_recv_cb(onDataRecv);

  // Optional: register peer (needed only if we ever send back)
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mainEspMac, 6);
  peer.channel = 0;      // same channel as current WiFi STA
  peer.encrypt = false;

  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("Peer add failed (optional for RX-only)");
  } else {
    Serial.println("ESP-NOW peer added");
  }
}

// ====================== LOOP ======================
void loop() {
  if (newNumber != -1) {
    currentNumber = newNumber;
    newNumber = -1;

    if (shouldPlay) {
      shouldPlay = false;
      player.play(currentNumber + 1);
    }
  }

  displayNumberOn7Seg(currentNumber);
}