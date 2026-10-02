// Thread-safe ring buffer of recent events, polled by the web UI and echoed to
// the serial console.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "pattern.h"

std::string jsonEscape(const std::string& s);

class EventLog {
 public:
  // kind: "info", "warn", "error", "ble", "midi", "learn"
  void add(const char* kind, const std::string& msg);
  void addReport(const HidReport& r, const std::string& effect);
  void addNote(uint32_t mappingId, bool on, const std::string& msg);

  // Appends events newer than `since` as comma-separated JSON objects.
  // Returns the newest sequence number.
  uint32_t since(uint32_t since, std::string& out);

 private:
  void push(const std::string& json);

  static constexpr size_t kEntries = 160;
  struct Entry {
    uint32_t seq = 0;
    std::string json;
  };
  Entry entries_[kEntries];
  uint32_t seq_ = 0;
  std::mutex mu_;
};

extern EventLog eventLog;
