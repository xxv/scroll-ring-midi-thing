#include "event_log.h"

#include <Arduino.h>

EventLog eventLog;

std::string jsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

void EventLog::push(const std::string& body) {
  std::lock_guard<std::mutex> lock(mu_);
  uint32_t seq = ++seq_;
  Entry& e = entries_[seq % kEntries];
  e.seq = seq;
  e.json = "{\"s\":" + std::to_string(seq) + ",\"t\":" + std::to_string(millis()) + "," + body + "}";
}

void EventLog::add(const char* kind, const std::string& msg) {
  Serial.printf("[%s] %s\n", kind, msg.c_str());
  push(std::string("\"k\":\"") + kind + "\",\"m\":\"" + jsonEscape(msg) + "\"");
}

void EventLog::addReport(const HidReport& r, const std::string& effect) {
  push("\"k\":\"hid\",\"r\":\"" + r.toString() + "\",\"m\":\"" + jsonEscape(effect) + "\"");
}

void EventLog::addNote(uint32_t mappingId, bool on, const std::string& msg) {
  push("\"k\":\"midi\",\"id\":" + std::to_string(mappingId) + ",\"on\":" + (on ? "true" : "false") +
       ",\"m\":\"" + jsonEscape(msg) + "\"");
}

uint32_t EventLog::since(uint32_t since, std::string& out) {
  std::lock_guard<std::mutex> lock(mu_);
  if (since > seq_) since = 0;  // client is from before a reboot
  uint32_t first = seq_ >= kEntries ? seq_ - kEntries + 1 : 1;
  if (since + 1 > first) first = since + 1;
  bool comma = false;
  for (uint32_t s = first; s <= seq_; s++) {
    const Entry& e = entries_[s % kEntries];
    if (e.seq != s) continue;
    if (comma) out += ',';
    out += e.json;
    comma = true;
  }
  return seq_;
}
