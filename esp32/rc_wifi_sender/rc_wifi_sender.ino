/*
 * rc_wifi_sender.ino  —  Step 7: i-BUS stick data -> Wi-Fi (UDP) at 50 Hz
 * Board: ESP32 Dev Module (ESP-32D, 38-pin, CP2102)
 * Receiver: FlySky FS-iA10B, i-BUS SERVO port (small top row, right group)
 *
 * WIRING  (i-BUS signal measured at 3.31 V -> safe, no divider)
 *   Receiver i-BUS SERVO  left  pin (GND)    -> ESP32 GND
 *   Receiver i-BUS SERVO  middle pin (+)     -> ESP32 5V (VIN)
 *   Receiver i-BUS SERVO  right pin (signal) -> ESP32 GPIO 16 (RX2)
 *
 * WHAT IT DOES
 *   - Reads i-BUS frames from the receiver (same decoder as Step 6).
 *   - Joins your laptop hotspot (station mode).
 *   - Every 20 ms (50 times a second) sends one UDP packet to the laptop:
 *
 *        RC,seq,ok,ch1,ch2,ch3,ch4,ch5,ch6
 *        e.g.  RC,1234,1,1504,1499,1000,1505,1000,1000
 *
 *     seq = packet number (laptop uses gaps to count lost packets)
 *     ok  = 1 if the receiver is sending fresh data, 0 if not
 *           (transmitter off / receiver unplugged) -> game can react safely
 *
 *   - If Wi-Fi drops, it keeps reading i-BUS and reconnects by itself.
 *     It never freezes waiting for Wi-Fi.
 *
 * BEFORE UPLOADING: copy secrets_example.h to a new file called secrets.h
 * in this folder, and put your hotspot name and password in it.
 * secrets.h is listed in .gitignore, so your password never goes to GitHub.
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include "secrets.h"   // WIFI_SSID and WIFI_PASS (not on GitHub)

// ---------- Settings you change ----------
const uint16_t LAPTOP_PORT = 4210;                // must match the Python side
const uint32_t SEND_INTERVAL_MS = 20;             // 20 ms = 50 packets/second
// -----------------------------------------

// ---------- i-BUS (same as Step 6) ----------
const int IBUS_RX_PIN = 16;
const int IBUS_TX_PIN = 17;     // unused
const int NUM_CH      = 14;

HardwareSerial IBus(2);
uint8_t  frame[32];
int      idx = 0;
uint16_t ch[NUM_CH];
uint32_t goodFrames = 0, badFrames = 0, lastFrameMs = 0;

bool ibusFeed(uint8_t b) {
  if (idx == 0 && b != 0x20) return false;          // wait for start byte
  if (idx == 1 && b != 0x40) {                      // second byte must be 0x40
    idx = (b == 0x20) ? 1 : 0;
    return false;
  }
  frame[idx++] = b;
  if (idx < 32) return false;
  idx = 0;

  uint16_t sum = 0xFFFF;                            // checksum check
  for (int i = 0; i < 30; i++) sum -= frame[i];
  if (sum != (uint16_t)(frame[30] | (frame[31] << 8))) {
    badFrames++;
    return false;
  }
  for (int i = 0; i < NUM_CH; i++) {
    ch[i] = (frame[2 + 2 * i] | (frame[3 + 2 * i] << 8)) & 0x0FFF;
  }
  goodFrames++;
  lastFrameMs = millis();
  return true;
}

// ---------- Wi-Fi + UDP ----------
WiFiUDP  udp;
uint32_t seq = 0;                 // packet counter
bool     wasConnected = false;    // to print only when the state CHANGES
bool     udpStarted = false;
uint32_t disconnectedSinceMs = 0;

void setup() {
  Serial.begin(115200);
  IBus.begin(115200, SERIAL_8N1, IBUS_RX_PIN, IBUS_TX_PIN);

  WiFi.mode(WIFI_STA);          // station mode: join the laptop hotspot
  WiFi.setSleep(false);         // keep the radio awake -> much lower latency
  WiFi.setAutoReconnect(true);  // let the Wi-Fi driver retry after drops
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.println("RC Wi-Fi sender started. Connecting to hotspot...");
  disconnectedSinceMs = millis();
}

void loop() {
  // 1) Always read the receiver first (never let Wi-Fi block this)
  while (IBus.available()) {
    ibusFeed(IBus.read());
  }

  // 2) Watch the Wi-Fi state and report changes
  bool connected = (WiFi.status() == WL_CONNECTED);
  if (connected && !wasConnected) {
    Serial.println("Wi-Fi connected!");
    Serial.print("  ESP32 IP:  "); Serial.println(WiFi.localIP());
    Serial.print("  Laptop IP: "); Serial.println(WiFi.gatewayIP());
    if (!udpStarted) { udp.begin(0); udpStarted = true; }
  }
  if (!connected && wasConnected) {
    Serial.println("Wi-Fi LOST - will keep trying...");
    disconnectedSinceMs = millis();
  }
  wasConnected = connected;

  // Backup plan: if still not connected after 10 s, kick a reconnect
  if (!connected && millis() - disconnectedSinceMs > 10000) {
    Serial.println("Still no Wi-Fi, retrying...");
    WiFi.reconnect();
    disconnectedSinceMs = millis();
  }

  // 3) Send one packet every 20 ms
  static uint32_t lastSend = 0;
  if (millis() - lastSend >= SEND_INTERVAL_MS) {
    lastSend = millis();
    seq++;

    // ok = 1 only if we got a good i-BUS frame in the last 200 ms
    int ok = (goodFrames > 0 && millis() - lastFrameMs < 200) ? 1 : 0;

    if (connected) {
      char msg[64];
      int len = snprintf(msg, sizeof(msg), "RC,%lu,%d,%u,%u,%u,%u,%u,%u",
                         (unsigned long)seq, ok,
                         ch[0], ch[1], ch[2], ch[3], ch[4], ch[5]);
      udp.beginPacket(WiFi.gatewayIP(), LAPTOP_PORT);
      udp.write((const uint8_t*)msg, len);
      udp.endPacket();
    }
  }

  // 4) Status on the Serial Monitor once per second
  static uint32_t lastPrint = 0;
  if (millis() - lastPrint >= 1000) {
    lastPrint = millis();
    Serial.printf("WiFi:%s  seq:%lu  CH1:%u CH2:%u CH3:%u CH4:%u  ibus good:%lu bad:%lu\n",
                  connected ? "OK " : "---", (unsigned long)seq,
                  ch[0], ch[1], ch[2], ch[3],
                  (unsigned long)goodFrames, (unsigned long)badFrames);
  }
}
