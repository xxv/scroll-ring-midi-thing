// Scroll Ring MIDI Thing
//
// Bluetooth LE "TikTok scroll ring" -> ESP32-S3 -> USB MIDI.
//
//   BLE task      receives HID input reports from the ring -> reportQueue
//   engine task   matches reports against learned mappings -> USB MIDI
//   loop()        Wi-Fi AP + web UI, debounced config saves
#include <Arduino.h>
#include <USB.h>
#include <USBMIDI.h>

#include <mutex>

#include "app_config.h"
#include "ble_hid_host.h"
#include "config_store.h"
#include "engine.h"
#include "event_log.h"
#include "web_ui.h"

namespace {

USBMIDI midi(USB_PRODUCT_NAME);
Engine engine;
std::mutex engineMutex;
QueueHandle_t reportQueue;

void logLine(const char* kind, const std::string& msg) { eventLog.add(kind, msg); }

void engineTask(void*) {
  for (;;) {
    HidReport r;
    bool got = xQueueReceive(reportQueue, &r, pdMS_TO_TICKS(2)) == pdTRUE;
    std::lock_guard<std::mutex> lock(engineMutex);
    uint32_t now = millis();
    if (got) engine.onReport(r, now);
    engine.tick(now);
  }
}

void loadConfig() {
  std::string json = storageLoad();
  if (json.empty()) {
    eventLog.add("info", "No saved mappings yet");
    return;
  }
  std::vector<Mapping> mappings;
  Settings settings;
  std::string err;
  if (!configFromJson(json, mappings, settings, err)) {
    eventLog.add("error", "Saved config unreadable (" + err + "), starting empty");
    return;
  }
  engine.replaceAll(std::move(mappings), settings);
  engine.clearDirty();
  eventLog.add("info", "Loaded " + std::to_string(engine.mappings().size()) + " mappings");
}

// Writes the config once it has been stable for a second, outside the engine
// lock so flash writes never delay MIDI.
void saveConfigIfDirty() {
  static uint32_t dirtySince = 0;
  std::string json;
  {
    std::lock_guard<std::mutex> lock(engineMutex);
    if (!engine.dirty()) {
      dirtySince = 0;
      return;
    }
    if (!dirtySince) dirtySince = millis() | 1;
    if (millis() - dirtySince < 1000) return;
    json = configToJson(engine);
    engine.clearDirty();
    dirtySince = 0;
  }
  if (!storageSave(json)) eventLog.add("error", "Saving config failed");
}

}  // namespace

void setup() {
  Serial.begin(115200);

  USB.productName(USB_PRODUCT_NAME);
  USB.manufacturerName(USB_MANUFACTURER_NAME);
  midi.begin();
  USB.begin();

  engine.midiOut = [](uint8_t status, uint8_t d1, uint8_t d2) {
    // USB-MIDI event packet: cable 0, code index = status high nibble.
    midiEventPacket_t p = {static_cast<uint8_t>(status >> 4), status, d1, d2};
    midi.writePacket(&p);
  };
  engine.log = logLine;
  engine.noteSent = [](const Mapping& m, bool on) {
    std::string msg = (m.name.empty() ? "#" + std::to_string(m.id) : m.name) + (on ? ": note on " : ": note off ") +
                      std::to_string(m.sentNumber) + " ch " + std::to_string(m.sentChannel + 1);
    eventLog.addNote(m.id, on, msg);
  };
  engine.reportSeen = [](const HidReport& r, const std::string& effect) { eventLog.addReport(r, effect); };

  if (!storageBegin()) eventLog.add("error", "LittleFS mount failed; mappings will not be saved");
  loadConfig();

  reportQueue = xQueueCreate(64, sizeof(HidReport));
  xTaskCreatePinnedToCore(engineTask, "engine", 8192, nullptr, 3, nullptr, 1);

  bleHost.begin(
      [](const HidReport& r) {
        if (xQueueSend(reportQueue, &r, 0) != pdTRUE) Serial.println("report queue full");
      },
      logLine,
      [](bool connected) {
        if (connected) return;
        std::lock_guard<std::mutex> lock(engineMutex);
        engine.allNotesOff();
      });

  webBegin(engine, engineMutex);
}

void loop() {
  webLoop();
  saveConfigIfDirty();
  delay(2);
}
