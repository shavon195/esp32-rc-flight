/*
 * ble_led_test.ino  —  Step 9: control the KSIPZE / Happy Lighting LED strip
 *                       from the ESP32 over Bluetooth Low Energy (BLE)
 * Board: ESP32 Dev Module (original ESP32 / ESP-32D)
 * Needs: ESP32 Arduino core 4.0+ (new BLE API: #include <BLE.h>)
 *
 * WIRING
 *   Nothing! Just the USB cable. The ESP32 talks to the LED controller
 *   by Bluetooth, the same way the Happy Lighting app on your phone does.
 *
 * BEFORE UPLOADING
 *   1. Close the Happy Lighting app on your phone (fully). The controller
 *      usually accepts only ONE Bluetooth connection at a time.
 *   2. Arduino IDE: Tools -> Partition Scheme -> "Huge APP (3MB No OTA/1MB SPIFFS)"
 *      (the Bluetooth library is big; the default partition may be too small)
 *
 * HOW IT WORKS
 *   - Scans for 5 s and lists nearby BLE devices.
 *   - Picks the one whose name starts with "QHM-" (your controller).
 *   - Connects to it as a CLIENT (the controller is the SERVER).
 *   - Looks for the "write" characteristic. This family of controllers
 *     (Triones / Happy Lighting / QHM) usually uses service FFD5 with
 *     characteristic FFD9; some variants use FFE5 / FFE9. We try both.
 *   - You type a letter in the Serial Monitor and it sends a command:
 *
 *        r = red      g = green    b = blue     c = cyan
 *        w = white    y = yellow   n = ON       o = OFF
 *
 *   Colour command (7 bytes):  56 RR GG BB 00 F0 AA
 *   Power on  (3 bytes):       CC 23 33
 *   Power off (3 bytes):       CC 24 33
 */

#include <BLE.h>
#include <vector>

const char* NAME_PREFIX = "QHM-";      // your controller's name starts with this

// Possible (service, characteristic) pairs for this controller family
static const BLEUUID SVC_A((uint16_t)0xFFD5);
static const BLEUUID CHR_A((uint16_t)0xFFD9);
static const BLEUUID SVC_B((uint16_t)0xFFE5);
static const BLEUUID CHR_B((uint16_t)0xFFE9);

BLEClient               client;        // handles: empty until created
BLERemoteCharacteristic ledChar;       // where we write commands

// ---------- Scan and return the controller (if found) ----------
bool findController(std::vector<BLEAdvertisedDevice>& found) {
  Serial.println("Scanning for 5 seconds...");
  BLEScan scan = BLE.getScan();
  scan.setActiveScan(true);                          // ask devices for their names
  BLEScan::Results results = scan.startBlocking(5000);

  for (BLEAdvertisedDevice dev : results) {
    String name = dev.getName();
    if (name.length() == 0) continue;                // skip nameless devices
    Serial.printf("  seen: %-20s %s  RSSI %d\n",
                  name.c_str(), dev.getAddress().toString().c_str(), dev.getRSSI());
    if (found.empty() && name.startsWith(NAME_PREFIX)) {
      found.push_back(dev);
    }
  }
  scan.clearResults();
  return !found.empty();
}

// ---------- Try one (service, characteristic) pair ----------
bool tryPair(const BLEUUID& svcUuid, const BLEUUID& chrUuid, const char* label) {
  BLERemoteService svc = client.getService(svcUuid);
  if (!svc) return false;
  BLERemoteCharacteristic chr = svc.getCharacteristic(chrUuid);
  if (!chr) return false;
  if (!(chr.canWrite() || chr.canWriteNoResponse())) return false;
  ledChar = chr;
  Serial.printf("Using %s\n", label);
  return true;
}

bool connectToController() {
  ledChar = BLERemoteCharacteristic();               // forget any old handle

  std::vector<BLEAdvertisedDevice> found;
  if (!findController(found)) {
    Serial.println("Controller not found. Is the Happy Lighting app closed?");
    return false;
  }
  Serial.printf(">> Found LED controller: %s\n", found[0].getName().c_str());

  Serial.println("Connecting...");
  BTStatus st = client.connect(found[0]);
  if (!st) {
    Serial.printf("Connection failed: %s\n", st.toString());
    return false;
  }
  client.discoverServices();                         // read the controller's menu
  Serial.println("Connected! Looking for the write characteristic...");

  if (tryPair(SVC_A, CHR_A, "service FFD5, characteristic FFD9")) return true;
  if (tryPair(SVC_B, CHR_B, "service FFE5, characteristic FFE9")) return true;

  Serial.println("Connected, but no known write characteristic found.");
  Serial.println("Check the services with the nRF Connect phone app and tell me what you see.");
  client.disconnect();
  return false;
}

void sendBytes(uint8_t* data, size_t len) {
  if (!ledChar || !client.isConnected()) {
    Serial.println("Not connected - reconnecting...");
    if (!connectToController()) return;
  }
  // Use "write without response" if the controller supports it (faster)
  bool withResponse = !ledChar.canWriteNoResponse();
  BTStatus st = ledChar.writeValue(data, len, withResponse);

  Serial.print(st ? "Sent:" : "Send FAILED:");
  for (size_t i = 0; i < len; i++) Serial.printf(" %02X", data[i]);
  Serial.println();
}

void setColor(uint8_t r, uint8_t g, uint8_t b) {
  uint8_t cmd[7] = {0x56, r, g, b, 0x00, 0xF0, 0xAA};
  sendBytes(cmd, sizeof(cmd));
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nBLE LED test. Commands: r g b c w y  n=on  o=off");

  BTStatus st = BLE.begin("ESP32-RC");               // start Bluetooth
  if (!st) {
    Serial.printf("BLE start failed: %s\n", st.toString());
    return;
  }
  Serial.printf("BLE stack: %s\n", BLE.getStackName());

  client = BLE.createClient();
  client.onDisconnect([](BLEClient &c, const BLEConnInfo &conn, uint8_t reason) {
    Serial.printf("Controller disconnected (reason 0x%02X)\n", reason);
  });

  connectToController();
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 'r': setColor(255, 0, 0);     break;
      case 'g': setColor(0, 255, 0);     break;
      case 'b': setColor(0, 0, 255);     break;
      case 'c': setColor(0, 255, 255);   break;   // cyan
      case 'w': setColor(255, 255, 255); break;
      case 'y': setColor(255, 180, 0);   break;
      case 'n': { uint8_t on[3]  = {0xCC, 0x23, 0x33}; sendBytes(on, 3);  break; }
      case 'o': { uint8_t off[3] = {0xCC, 0x24, 0x33}; sendBytes(off, 3); break; }
      default: break;                              // ignore newlines etc.
    }
  }
}
