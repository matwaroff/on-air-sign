#pragma once

// Copy this file to include/secrets.h and fill in your Wi-Fi details.
// include/secrets.h is ignored by git.

#define WIFI_SSID "your-wifi-name"
#define WIFI_PASSWORD "your-wifi-password"

// Used only if the board cannot join Wi-Fi and falls back to setup AP mode.
// Must be at least 8 characters for WPA/WPA2.
#define ON_AIR_AP_PASSWORD "onair1234"
