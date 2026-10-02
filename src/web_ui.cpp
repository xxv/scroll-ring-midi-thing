#include "web_ui.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <USB.h>
#include <WebServer.h>
#include <WiFi.h>

#include "app_config.h"
#include "ble_hid_host.h"
#include "config_store.h"
#include "event_log.h"

// web/index.html, embedded by board_build.embed_txtfiles (NUL-terminated).
extern const char index_html_start[] asm("_binary_web_index_html_start");
extern const char index_html_end[] asm("_binary_web_index_html_end");

namespace {

WebServer server(80);
DNSServer dns;
Engine* engine = nullptr;
std::mutex* engineMutex = nullptr;

using Lock = std::lock_guard<std::mutex>;

void sendJson(const std::string& body, int code = 200) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body.c_str());
}

void sendOk() { sendJson("{\"ok\":true}"); }

void sendError(int code, const std::string& msg) {
  sendJson("{\"ok\":false,\"error\":\"" + jsonEscape(msg) + "\"}", code);
}

// Parses the POST body into `doc`. Sends a 400 and returns false on bad JSON.
bool readBody(JsonDocument& doc) {
  String body = server.arg("plain");
  if (body.length() == 0) {
    doc.to<JsonObject>();
    return true;
  }
  if (deserializeJson(doc, body)) {
    sendError(400, "invalid JSON body");
    return false;
  }
  return true;
}

std::string hex4(uint16_t v) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%04x", v);
  return buf;
}

// --- Pages -------------------------------------------------------------------

void handleIndex() {
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html; charset=utf-8", index_html_start, index_html_end - index_html_start - 1);
}

// Phones probe random URLs to detect captive portals; send them to the UI.
void handleNotFound() {
  String host = server.hostHeader();
  String ip = WiFi.softAPIP().toString();
  if (host.length() && host != ip) {
    server.sendHeader("Location", "http://" + ip + "/", true);
    server.send(302, "text/plain", "");
    return;
  }
  server.send(404, "text/plain", "Not found");
}

// --- State -------------------------------------------------------------------

void handleState() {
  JsonDocument doc;
  BleStatus b = bleHost.status();
  doc["brev"] = bleHost.revision();
  doc["uptime"] = millis();
  doc["usb"] = static_cast<bool>(USB);

  JsonObject ble = doc["ble"].to<JsonObject>();
  ble["state"] = bleStateName(b.state);
  ble["hasTarget"] = b.hasTarget;
  ble["autoConnect"] = b.autoConnect;
  ble["addr"] = b.targetAddr;
  ble["addrType"] = b.targetType;
  ble["name"] = b.targetName;
  if (b.battery >= 0) ble["battery"] = b.battery;
  ble["error"] = b.lastError;
  ble["reportMap"] = b.reportMapHex;
  JsonArray srcs = ble["sources"].to<JsonArray>();
  for (const auto& s : b.sources) {
    JsonObject o = srcs.add<JsonObject>();
    o["handle"] = hex4(s.handle);
    o["reportId"] = s.reportId;
    o["kind"] = s.kind;
  }
  JsonArray scan = ble["scan"].to<JsonArray>();
  for (const auto& s : b.scan) {
    JsonObject o = scan.add<JsonObject>();
    o["addr"] = s.addr;
    o["addrType"] = s.addrType;
    o["name"] = s.name;
    o["rssi"] = s.rssi;
    o["hid"] = s.hid;
    if (s.appearance) o["appearance"] = s.appearance;
  }

  {
    Lock lock(*engineMutex);
    doc["rev"] = engine->revision();
    settingsToJson(engine->settings, doc["settings"].to<JsonObject>());
    JsonArray maps = doc["mappings"].to<JsonArray>();
    for (const auto& m : engine->mappings()) {
      JsonObject o = maps.add<JsonObject>();
      mappingToJson(m, o);
      o["active"] = m.active;
    }
    const LearnState& l = engine->learn();
    JsonObject lj = doc["learn"].to<JsonObject>();
    lj["active"] = l.active;
    lj["id"] = l.mappingId;
    lj["want"] = engine->settings.learnSamples;
    lj["capturing"] = l.capturing;
    lj["currentLen"] = l.current.size();
    JsonArray samples = lj["samples"].to<JsonArray>();
    for (const auto& s : l.samples) {
      JsonArray sa = samples.add<JsonArray>();
      for (const auto& r : s) sa.add(r.toString());
    }
    lj["resultId"] = l.resultMappingId;
    JsonArray notes = lj["notes"].to<JsonArray>();
    for (const auto& n : l.notes) notes.add(n);
  }

  std::string out;
  serializeJson(doc, out);
  sendJson(out);
}

void handleEvents() {
  uint32_t since = server.arg("since").toInt();
  std::string events;
  uint32_t seq = eventLog.since(since, events);
  uint32_t rev;
  {
    Lock lock(*engineMutex);
    rev = engine->revision();
  }
  sendJson("{\"seq\":" + std::to_string(seq) + ",\"rev\":" + std::to_string(rev) +
           ",\"brev\":" + std::to_string(bleHost.revision()) + ",\"events\":[" + events + "]}");
}

// --- Bluetooth -----------------------------------------------------------------

void handleScan() {
  bleHost.requestScan();
  sendOk();
}

void handleConnect() {
  JsonDocument doc;
  if (!readBody(doc)) return;
  std::string addr = doc["addr"] | "";
  if (addr.size() != 17) return sendError(400, "addr must look like aa:bb:cc:dd:ee:ff");
  bleHost.requestConnect(addr, doc["addrType"] | 0, doc["name"] | "");
  sendOk();
}

void handleDisconnect() {
  bleHost.requestDisconnect();
  sendOk();
}

void handleForget() {
  bleHost.requestForget();
  sendOk();
}

// --- Mappings ------------------------------------------------------------------

uint8_t firstFreeNote(const Engine& e) {
  for (int n = 60; n < 128; n++) {
    bool used = false;
    for (const auto& m : e.mappings()) used = used || m.number == n;
    if (!used) return n;
  }
  return 60;
}

void handleMappingSave() {
  JsonDocument doc;
  if (!readBody(doc)) return;
  JsonObjectConst in = doc.as<JsonObjectConst>();
  uint32_t id = in["id"] | 0;
  std::string err;
  Lock lock(*engineMutex);
  if (id == 0) {
    Mapping m;
    m.number = firstFreeNote(*engine);
    m.name = "Button " + std::to_string(engine->mappings().size() + 1);
    if (!mappingFromJson(in, m, err)) return sendError(400, err);
    id = engine->addMapping(m);
    if (!id) return sendError(400, "too many mappings");
  } else {
    Mapping* existing = engine->find(id);
    if (!existing) return sendError(404, "no such mapping");
    Mapping m = *existing;
    if (!mappingFromJson(in, m, err)) return sendError(400, err);
    m.id = id;
    engine->updateMapping(m);
  }
  sendJson("{\"ok\":true,\"id\":" + std::to_string(id) + "}");
}

uint32_t bodyId(JsonDocument& doc) { return doc["id"] | 0; }

void handleMappingDelete() {
  JsonDocument doc;
  if (!readBody(doc)) return;
  Lock lock(*engineMutex);
  if (!engine->removeMapping(bodyId(doc))) return sendError(404, "no such mapping");
  sendOk();
}

void handleMappingTest() {
  JsonDocument doc;
  if (!readBody(doc)) return;
  Lock lock(*engineMutex);
  if (!engine->testMapping(bodyId(doc), millis())) return sendError(404, "no such mapping");
  sendOk();
}

void handleMappingPick() {
  JsonDocument doc;
  if (!readBody(doc)) return;
  Lock lock(*engineMutex);
  if (!engine->pickTrigger(bodyId(doc), doc["index"] | -1)) return sendError(400, "bad mapping or index");
  sendOk();
}

// --- Learning ------------------------------------------------------------------

void handleLearnStart() {
  JsonDocument doc;
  if (!readBody(doc)) return;
  Lock lock(*engineMutex);
  if (!engine->startLearn(bodyId(doc))) return sendError(404, "no such mapping");
  sendOk();
}

void handleLearnFinish() {
  Lock lock(*engineMutex);
  engine->finishLearn();
  sendOk();
}

void handleLearnCancel() {
  Lock lock(*engineMutex);
  engine->cancelLearn();
  sendOk();
}

// --- Settings / config -----------------------------------------------------------

void handleSettings() {
  JsonDocument doc;
  if (!readBody(doc)) return;
  Lock lock(*engineMutex);
  Settings s = engine->settings;
  settingsFromJson(doc.as<JsonObjectConst>(), s);
  engine->setSettings(s);
  sendOk();
}

void handlePanic() {
  Lock lock(*engineMutex);
  engine->allNotesOff();
  if (engine->midiOut) {
    for (uint8_t ch = 0; ch < 16; ch++) engine->midiOut(0xB0 | ch, 123, 0);  // All Notes Off
  }
  eventLog.add("midi", "Panic: all notes off");
  sendOk();
}

void handleConfigGet() {
  std::string json;
  {
    Lock lock(*engineMutex);
    json = configToJson(*engine);
  }
  server.sendHeader("Content-Disposition", "attachment; filename=\"scroll-ring-midi.json\"");
  sendJson(json);
}

void handleConfigPut() {
  std::vector<Mapping> mappings;
  Settings settings;
  std::string err;
  if (!configFromJson(server.arg("plain").c_str(), mappings, settings, err)) return sendError(400, err);
  Lock lock(*engineMutex);
  engine->replaceAll(std::move(mappings), settings);
  eventLog.add("info", "Imported " + std::to_string(engine->mappings().size()) + " mappings");
  sendOk();
}

}  // namespace

void webBegin(Engine& e, std::mutex& mu) {
  engine = &e;
  engineMutex = &mu;

  char ssid[40];
  uint64_t mac = ESP.getEfuseMac();
  snprintf(ssid, sizeof(ssid), "%s-%02X%02X", AP_SSID_PREFIX, static_cast<uint8_t>(mac >> 32),
           static_cast<uint8_t>(mac >> 40));
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, strlen(AP_PASSWORD) ? AP_PASSWORD : nullptr);
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", WiFi.softAPIP());
  eventLog.add("info", std::string("Wi-Fi AP \"") + ssid + "\" up at http://" + WiFi.softAPIP().toString().c_str() + "/");

  server.on("/", HTTP_GET, handleIndex);
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/events", HTTP_GET, handleEvents);
  server.on("/api/ble/scan", HTTP_POST, handleScan);
  server.on("/api/ble/connect", HTTP_POST, handleConnect);
  server.on("/api/ble/disconnect", HTTP_POST, handleDisconnect);
  server.on("/api/ble/forget", HTTP_POST, handleForget);
  server.on("/api/mapping", HTTP_POST, handleMappingSave);
  server.on("/api/mapping/delete", HTTP_POST, handleMappingDelete);
  server.on("/api/mapping/test", HTTP_POST, handleMappingTest);
  server.on("/api/mapping/pick", HTTP_POST, handleMappingPick);
  server.on("/api/learn/start", HTTP_POST, handleLearnStart);
  server.on("/api/learn/finish", HTTP_POST, handleLearnFinish);
  server.on("/api/learn/cancel", HTTP_POST, handleLearnCancel);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.on("/api/panic", HTTP_POST, handlePanic);
  server.on("/api/config", HTTP_GET, handleConfigGet);
  server.on("/api/config", HTTP_POST, handleConfigPut);
  server.onNotFound(handleNotFound);
  server.begin();
}

void webLoop() {
  dns.processNextRequest();
  server.handleClient();
}
