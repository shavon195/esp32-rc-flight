/*
 * udp_hello_esp32.ino  —  Step 4: first UDP message from ESP32 to laptop
 * Board: ESP32 Dev Module (your ESP-32D board, 38-pin, CP2102)
 *
 * WIRING
 *   Nothing! Just the USB cable. No receiver yet.
 *
 * WHAT IT DOES
 *   1. Joins your laptop's hotspot in STATION mode (like a phone would).
 *   2. Prints the IP addresses it was given.
 *   3. Sends "hello #1", "hello #2", ... once per second over UDP
 *      to the laptop, on port 4210.
 *
 * WHY THE GATEWAY IP?
 *   When your laptop runs a hotspot, the laptop IS the router.
 *   The "gateway" is the router's address, so gatewayIP() is the
 *   laptop's address on the hotspot (usually 192.168.137.1).
 *   That means we don't have to type the laptop's IP by hand.
 *
 * BEFORE UPLOADING: copy secrets_example.h to a new file called secrets.h
 * in this folder, and put your hotspot name and password in it.
 * secrets.h is listed in .gitignore, so your password never goes to GitHub.
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include "secrets.h"   // WIFI_SSID and WIFI_PASS (not on GitHub)

// ---------- Settings you change ----------
const uint16_t LAPTOP_PORT = 4210;                // the "door number" on the laptop
// -----------------------------------------

WiFiUDP udp;              // object that sends UDP packets
uint32_t counter = 0;     // counts messages so we can spot lost ones

void connectWiFi() {
  Serial.print("Connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);              // STATION mode: join an existing network
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // Wait until connected, printing a dot every half second
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("Connected!");
  Serial.print("  My IP (ESP32):      "); Serial.println(WiFi.localIP());
  Serial.print("  Gateway (laptop):   "); Serial.println(WiFi.gatewayIP());
  Serial.print("  Signal (RSSI dBm):  "); Serial.println(WiFi.RSSI());
}

void setup() {
  Serial.begin(115200);
  delay(500);
  connectWiFi();
  udp.begin(0);   // open a UDP socket on any free local port (we only send)
}

void loop() {
  // If Wi-Fi dropped (hotspot turned off, out of range), reconnect.
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi lost! Reconnecting...");
    connectWiFi();
  }

  counter++;

  // Build and send one UDP packet
  udp.beginPacket(WiFi.gatewayIP(), LAPTOP_PORT);  // where to send it
  udp.printf("hello #%lu", (unsigned long)counter); // what's inside
  udp.endPacket();                                  // send it now

  Serial.printf("Sent: hello #%lu\n", (unsigned long)counter);
  delay(1000);   // once per second
}
