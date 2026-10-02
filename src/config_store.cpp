#include "config_store.h"

#include <LittleFS.h>

#include "app_config.h"

namespace {

bool patternField(JsonObjectConst in, const char* key, Pattern& out, std::string& err) {
  JsonVariantConst v = in[key];
  if (v.isNull()) return true;
  if (!Pattern::parse(v.as<std::string>(), out)) {
    err = std::string("bad ") + key + " pattern: \"" + v.as<std::string>() + "\"";
    return false;
  }
  return true;
}

template <typename T>
void clampedField(JsonObjectConst in, const char* key, T& out, long lo, long hi) {
  JsonVariantConst v = in[key];
  if (!v.is<long>()) return;
  long x = v.as<long>();
  out = static_cast<T>(x < lo ? lo : (x > hi ? hi : x));
}

}  // namespace

void mappingToJson(const Mapping& m, JsonObject o) {
  o["id"] = m.id;
  o["name"] = m.name;
  o["enabled"] = m.enabled;
  o["action"] = midiActionName(m.action);
  o["channel"] = m.channel;
  o["number"] = m.number;
  o["velocity"] = m.velocity;
  o["trigger"] = m.trigger.toString();
  o["release"] = m.release.toString();
  o["releaseMode"] = releaseModeName(m.releaseMode);
  o["holdMs"] = m.holdMs;
  o["triggerIndex"] = m.triggerIndex;
  o["triggerAuto"] = m.triggerAuto;
  JsonArray g = o["gesture"].to<JsonArray>();
  for (const auto& p : m.gesture) g.add(p.toString());
}

bool mappingFromJson(JsonObjectConst in, Mapping& m, std::string& err) {
  if (in["id"].is<uint32_t>()) m.id = in["id"].as<uint32_t>();
  if (in["name"].is<const char*>()) m.name = in["name"].as<std::string>().substr(0, 40);
  if (in["enabled"].is<bool>()) m.enabled = in["enabled"].as<bool>();
  if (in["action"].is<const char*>() && !parseMidiAction(in["action"].as<std::string>(), m.action)) {
    err = "unknown action";
    return false;
  }
  clampedField(in, "channel", m.channel, 1, 16);
  clampedField(in, "number", m.number, 0, 127);
  clampedField(in, "velocity", m.velocity, 1, 127);
  clampedField(in, "holdMs", m.holdMs, 1, 60000);
  if (in["releaseMode"].is<const char*>() &&
      !parseReleaseMode(in["releaseMode"].as<std::string>(), m.releaseMode)) {
    err = "unknown releaseMode";
    return false;
  }

  Pattern trig = m.trigger;
  if (!patternField(in, "trigger", trig, err)) return false;
  if (!patternField(in, "release", m.release, err)) return false;

  if (in["gesture"].is<JsonArrayConst>()) {
    m.gesture.clear();
    for (JsonVariantConst v : in["gesture"].as<JsonArrayConst>()) {
      Pattern p;
      if (Pattern::parse(v.as<std::string>(), p) && !p.empty() && m.gesture.size() < kMaxGestureLen) {
        m.gesture.push_back(p);
      }
    }
  }
  if (in["triggerIndex"].is<int>()) m.triggerIndex = in["triggerIndex"].as<int>();
  if (in["triggerAuto"].is<bool>()) m.triggerAuto = in["triggerAuto"].as<bool>();

  // A trigger that no longer matches its gesture slot was hand-edited.
  if (trig.toString() != m.trigger.toString()) {
    bool fromGesture = m.triggerIndex >= 0 && m.triggerIndex < static_cast<int>(m.gesture.size()) &&
                       m.gesture[m.triggerIndex].toString() == trig.toString();
    if (!fromGesture) {
      m.triggerIndex = -1;
      m.triggerAuto = false;
    }
  }
  m.trigger = trig;
  if (m.triggerIndex >= static_cast<int>(m.gesture.size())) m.triggerIndex = -1;
  return true;
}

void settingsToJson(const Settings& s, JsonObject o) {
  o["gestureGapMs"] = s.gestureGapMs;
  o["learnGapMs"] = s.learnGapMs;
  o["learnTailMs"] = s.learnTailMs;
  o["learnSamples"] = s.learnSamples;
}

void settingsFromJson(JsonObjectConst in, Settings& s) {
  clampedField(in, "gestureGapMs", s.gestureGapMs, 20, 5000);
  clampedField(in, "learnGapMs", s.learnGapMs, 50, 5000);
  clampedField(in, "learnTailMs", s.learnTailMs, 0, 10000);
  clampedField(in, "learnSamples", s.learnSamples, 1, kMaxLearnSamples);
}

std::string configToJson(const Engine& engine) {
  JsonDocument doc;
  doc["version"] = 1;
  settingsToJson(engine.settings, doc["settings"].to<JsonObject>());
  JsonArray arr = doc["mappings"].to<JsonArray>();
  for (const auto& m : engine.mappings()) mappingToJson(m, arr.add<JsonObject>());
  std::string out;
  serializeJsonPretty(doc, out);
  return out;
}

bool configFromJson(const std::string& json, std::vector<Mapping>& mappings, Settings& settings, std::string& err) {
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, json);
  if (e) {
    err = std::string("invalid JSON: ") + e.c_str();
    return false;
  }
  if (!doc["mappings"].is<JsonArrayConst>()) {
    err = "missing \"mappings\" array";
    return false;
  }
  settings = Settings();
  if (doc["settings"].is<JsonObjectConst>()) settingsFromJson(doc["settings"].as<JsonObjectConst>(), settings);
  mappings.clear();
  for (JsonObjectConst o : doc["mappings"].as<JsonArrayConst>()) {
    Mapping m;
    if (!mappingFromJson(o, m, err)) return false;
    if (mappings.size() < kMaxMappings) mappings.push_back(m);
  }
  return true;
}

bool storageBegin() {
  return LittleFS.begin(true);  // formats on first boot
}

bool storageSave(const std::string& json) {
  const char* tmp = CONFIG_PATH ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  size_t n = f.write(reinterpret_cast<const uint8_t*>(json.data()), json.size());
  f.close();
  if (n != json.size()) return false;
  LittleFS.remove(CONFIG_PATH);
  return LittleFS.rename(tmp, CONFIG_PATH);
}

std::string storageLoad() {
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) return "";
  std::string s;
  s.reserve(f.size());
  while (f.available()) {
    char buf[256];
    size_t n = f.readBytes(buf, sizeof(buf));
    s.append(buf, n);
  }
  return s;
}
