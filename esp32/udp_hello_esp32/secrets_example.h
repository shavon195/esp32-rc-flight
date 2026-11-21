// secrets_example.h  —  template for your Wi-Fi details
//
// 1. Copy this file and name the copy:  secrets.h   (same folder)
// 2. Put your real hotspot name and password in secrets.h
// 3. Never commit secrets.h — it is listed in .gitignore
//
// The laptop hotspot must be 2.4 GHz (the ESP32 cannot use 5 GHz).

#pragma once

const char* WIFI_SSID = "YOUR_HOTSPOT_NAME";
const char* WIFI_PASS = "YOUR_HOTSPOT_PASSWORD";
