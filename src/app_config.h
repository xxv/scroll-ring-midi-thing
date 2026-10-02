// Compile-time settings. Override any of these with build_flags in
// platformio.ini, e.g. -DAP_PASSWORD=\"something-else\"
#pragma once

#ifndef AP_SSID_PREFIX
#define AP_SSID_PREFIX "ScrollRingMIDI"  // the chip ID's last 4 hex digits are appended
#endif

#ifndef AP_PASSWORD
#define AP_PASSWORD "ringmidi"  // 8+ chars, or "" for an open network
#endif

#ifndef USB_PRODUCT_NAME
#define USB_PRODUCT_NAME "Scroll Ring MIDI"
#endif

#ifndef USB_MANUFACTURER_NAME
#define USB_MANUFACTURER_NAME "scroll-ring-midi-thing"
#endif

#ifndef BLE_DEVICE_NAME
#define BLE_DEVICE_NAME "ScrollRingMIDI"
#endif

#define CONFIG_PATH "/config.json"
