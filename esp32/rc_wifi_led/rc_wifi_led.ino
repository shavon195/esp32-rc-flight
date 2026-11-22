/*
 * rc_wifi_led.ino  —  Step 10: sticks -> Wi-Fi, and game events -> room LEDs
 * Board: ESP32 Dev Module (original ESP32 / ESP-32D)
 * Needs: ESP32 Arduino core 4.0+ (new BLE API: #include <BLE.h>)
 *        Tools -> Partition Scheme -> "Huge APP (3MB No OTA/1MB SPIFFS)"
 *
 * WIRING  (same as rc_wifi_sender)
 *   Receiver i-BUS SERVO  left  pin (GND)    -> ESP32 GND
 *   Receiver i-BUS SERVO  middle pin (+)     -> ESP32 5V (VIN)
 *   Receiver i-BUS SERVO  right pin (signal) -> ESP32 GPIO 16 (RX2)
 *   LED controller: no wires — Bluetooth (BLE). Keep the ESP32 within a few
 *   metres of the controller box, and close the Happy Lighting app.
 *
 * WHAT IT DOES — two jobs running at the same time:
 *
 *   Core 1 (loop):  read i-BUS -> send  RC,seq,ok,ch1..ch6  to the laptop
 *                   50 times/s over UDP, and listen for game events:
 *                       EVT,id,CRASH  -> queue RED
 *                       EVT,id,START  -> queue CYAN
 *
 *   Core 0 (bleTask): keep a Bluetooth connection to the LED controller
 *                   and send it whatever colour is waiting in the queue.
 *                   Connecting can take seconds, so it runs in its own
 *                   task: the sticks never freeze while Bluetooth is busy.
 *
 *   ONE UDP socket on port 4211 does both directions: stick packets go
 *   OUT from port 4211, and the game replies to the port they came from.
 *
 * WI-FI + BLUETOOTH SHARE ONE RADIO
 *   The ESP32 time-shares its antenna between them ("coexistence").
 *   That needs Wi-Fi modem sleep ON, so this sketch does NOT call
 *   WiFi.setSleep(false). Watch the game's pkt/s and loss numbers to
 *   see what that costs compared with rc_wifi_sender.
 *
 * BEFORE UPLOADING: create a tab called secrets.h with your hotspot details
 * (copy secrets_example.h). secrets.h is in .gitignore, never on GitHub.
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include <BLE.h>
#include <vector>
#include "secrets.h"   // WIFI_SSID and WIFI_PASS (not on GitHub)

// ---------- Settings ----------
const uint16_t LAPTOP_PORT      = 4210;   // game listens here
const uint16_t ESP_PORT         = 4211;   // we send from / listen on this port
const uint32_t SEND_INTERVAL_MS = 20;     // 50 packets/second
const char*    LED_NAME_PREFIX  = "QHM-"; // your LED controller's name starts with this
// ------------------------------

// ================= i-BUS (same as before) =================
const int IBUS_RX_PIN = 16, IBUS_TX_PIN = 17, NUM_CH = 14;
HardwareSerial IBus(2);
uint8_t  frame[32];
int      idx = 0;
uint16_t ch[NUM_CH];
uint32_t goodFrames = 0, badFrames = 0, lastFrameMs = 0;

bool ibusFeed(uint8_t b) {
  if (idx == 0 && b != 0x20) return false;
  if (idx == 1 && b != 0x40) { idx = (b == 0x20) ? 1 : 0; return false; }
  frame[idx++] = b;
  if (idx < 32) return false;
  idx = 0;
  uint16_t sum = 0xFFFF;
  for (int i = 0; i < 30; i++) sum -= frame[i];
  if (sum != (uint16_t)(frame[30] | (frame[31] << 8))) { badFrames++; return false; }
  for (int i = 0; i < NUM_CH; i++)
    ch[i] = (frame[2 + 2 * i] | (frame[3 + 2 * i] << 8)) & 0x0FFF;
  goodFrames++;
  lastFrameMs = millis();
  return true;
}

// ================= LED queue (shared by both tasks) =================
struct LedCmd { uint8_t r, g, b; };
QueueHandle_t ledQueue;          // holds ONE colour: the newest one wins
volatile bool ledConnected = false;

void requestColor(uint8_t r, uint8_t g, uint8_t b) {
  LedCmd c = {r, g, b};
  xQueueOverwrite(ledQueue, &c); // replace whatever was waiting
}

// ================= Bluetooth task (core 0) =================
static const BLEUUID SVC_A((uint16_t)0xFFD5), CHR_A((uint16_t)0xFFD9);
static const BLEUUID SVC_B((uint16_t)0xFFE5), CHR_B((uint16_t)0xFFE9);
BLEClient               client;
BLERemoteCharacteristic ledChar;

bool tryPair(const BLEUUID& s, const BLEUUID& c) {
  BLERemoteService svc = client.getService(s);
  if (!svc) return false;
  BLERemoteCharacteristic chr = svc.getCharacteristic(c);
  if (!chr || !(chr.canWrite() || chr.canWriteNoResponse())) return false;
  ledChar = chr;
  return true;
}

bool connectToLeds() {
  ledChar = BLERemoteCharacteristic();
  BLEScan scan = BLE.getScan();
  scan.setActiveScan(true);
  BLEScan::Results results = scan.startBlocking(4000);

  std::vector<BLEAdvertisedDevice> found;
  for (BLEAdvertisedDevice dev : results) {
    if (dev.getName().startsWith(LED_NAME_PREFIX)) { found.push_back(dev); break; }
  }
  scan.clearResults();
  if (found.empty()) { Serial.println("[BLE] LED controller not found"); return false; }

  Serial.printf("[BLE] Connecting to %s (RSSI %d)...\n",
                found[0].getName().c_str(), found[0].getRSSI());
  BTStatus st = client.connect(found[0]);
  if (!st) { Serial.printf("[BLE] Connect failed: %s\n", st.toString()); return false; }
  client.discoverServices();
  if (tryPair(SVC_A, CHR_A) || tryPair(SVC_B, CHR_B)) {
    Serial.println("[BLE] LED controller ready");
    return true;
  }
  Serial.println("[BLE] No write characteristic found");
  client.disconnect();
  return false;
}

void bleTask(void*) {
  BTStatus st = BLE.begin("ESP32-RC");
  if (!st) { Serial.printf("[BLE] start failed: %s\n", st.toString()); vTaskDelete(NULL); }
  client = BLE.createClient();
  client.onDisconnect([](BLEClient &c, const BLEConnInfo &conn, uint8_t reason) {
    Serial.printf("[BLE] LED controller disconnected (0x%02X)\n", reason);
  });

  for (;;) {
    // 1) Make sure we're connected (retry every 5 s if not)
    if (!ledChar || !client.isConnected()) {
      ledConnected = false;
      if (!connectToLeds()) { vTaskDelay(pdMS_TO_TICKS(5000)); continue; }
      ledConnected = true;
    }
    // 2) Wait up to 200 ms for a colour request, then send it
    LedCmd c;
    if (xQueueReceive(ledQueue, &c, pdMS_TO_TICKS(200)) == pdTRUE) {
      uint8_t cmd[7] = {0x56, c.r, c.g, c.b, 0x00, 0xF0, 0xAA};
      BTStatus ws = ledChar.writeValue(cmd, sizeof(cmd), !ledChar.canWriteNoResponse());
      Serial.printf("[BLE] colour %02X%02X%02X %s\n", c.r, c.g, c.b, ws ? "sent" : "FAILED");
    }
  }
}

// ================= Wi-Fi + UDP (core 1, in loop) =================
WiFiUDP  udp;
uint32_t seq = 0;
bool     wasConnected = false, udpStarted = false;
uint32_t disconnectedSinceMs = 0;
uint32_t lastEventId = 0;        // ignore repeated copies of the same event
uint32_t eventsReceived = 0;

void handleEvent(char* msg) {
  // Expected: EVT,<id>,<NAME>
  char* tag  = strtok(msg, ",");
  char* id   = strtok(NULL, ",");
  char* name = strtok(NULL, ",\r\n");
  if (!tag || !id || !name || strcmp(tag, "EVT") != 0) return;

  uint32_t eid = strtoul(id, nullptr, 10);
  if (eid == lastEventId) return;   // a duplicate copy: already handled
  lastEventId = eid;
  eventsReceived++;

  Serial.printf("[GAME] event %lu: %s\n", (unsigned long)eid, name);
  if      (strcmp(name, "CRASH") == 0) requestColor(255, 0, 0);    // red
  else if (strcmp(name, "START") == 0) requestColor(0, 255, 255);  // cyan
}

void setup() {
  Serial.begin(115200);
  IBus.begin(115200, SERIAL_8N1, IBUS_RX_PIN, IBUS_TX_PIN);

  ledQueue = xQueueCreate(1, sizeof(LedCmd));
  // Bluetooth work runs on core 0 so loop() (core 1) never waits for it
  xTaskCreatePinnedToCore(bleTask, "bleTask", 8192, NULL, 1, NULL, 0);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  // NOTE: no WiFi.setSleep(false) here — Wi-Fi + Bluetooth need modem sleep ON
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.println("RC Wi-Fi + LED started. Connecting to hotspot...");
  disconnectedSinceMs = millis();
}

void loop() {
  // 1) Receiver
  while (IBus.available()) ibusFeed(IBus.read());

  // 2) Wi-Fi state
  bool connected = (WiFi.status() == WL_CONNECTED);
  if (connected && !wasConnected) {
    Serial.printf("Wi-Fi connected! ESP32 %s -> laptop %s\n",
                  WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str());
    if (!udpStarted) { udp.begin(ESP_PORT); udpStarted = true; }
  }
  if (!connected && wasConnected) {
    Serial.println("Wi-Fi LOST - will keep trying...");
    disconnectedSinceMs = millis();
  }
  wasConnected = connected;
  if (!connected && millis() - disconnectedSinceMs > 10000) {
    WiFi.reconnect();
    disconnectedSinceMs = millis();
  }

  // 3) Incoming game events
  if (connected && udpStarted) {
    int size = udp.parsePacket();
    if (size > 0) {
      char buf[64];
      int n = udp.read(buf, sizeof(buf) - 1);
      if (n > 0) { buf[n] = '\0'; handleEvent(buf); }
    }
  }

  // 4) Stick packets, 50 per second
  static uint32_t lastSend = 0;
  if (millis() - lastSend >= SEND_INTERVAL_MS) {
    lastSend = millis();
    seq++;
    int ok = (goodFrames > 0 && millis() - lastFrameMs < 200) ? 1 : 0;
    if (connected && udpStarted) {
      char msg[64];
      int len = snprintf(msg, sizeof(msg), "RC,%lu,%d,%u,%u,%u,%u,%u,%u",
                         (unsigned long)seq, ok, ch[0], ch[1], ch[2], ch[3], ch[4], ch[5]);
      udp.beginPacket(WiFi.gatewayIP(), LAPTOP_PORT);
      udp.write((const uint8_t*)msg, len);
      udp.endPacket();
    }
  }

  // 5) Status once per second
  static uint32_t lastPrint = 0;
  if (millis() - lastPrint >= 1000) {
    lastPrint = millis();
    Serial.printf("WiFi:%s LED:%s seq:%lu events:%lu CH1:%u CH2:%u CH3:%u ibus bad:%lu\n",
                  connected ? "OK " : "---", ledConnected ? "OK " : "---",
                  (unsigned long)seq, (unsigned long)eventsReceived,
                  ch[0], ch[1], ch[2], (unsigned long)badFrames);
  }
}
