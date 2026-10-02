// Bluetooth LE HID host ("HID over GATT" central) for the ring.
//
// Scans for devices, pairs/bonds with the chosen one, subscribes to every HID
// input report it exposes and forwards each notification as a raw HidReport.
// The chosen device is remembered and reconnected automatically.
//
// All blocking BLE work happens on a dedicated task; the public methods only
// queue requests and are safe to call from any task.
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "pattern.h"

struct BleScanEntry {
  std::string addr;
  uint8_t addrType = 0;
  std::string name;
  int rssi = 0;
  bool hid = false;  // advertises the HID service or a HID appearance
  uint16_t appearance = 0;
};

struct BleReportSource {
  uint16_t handle = 0;
  uint8_t reportId = 0;
  uint8_t reportType = 0;  // 1 = input; 0 = unknown/boot
  const char* kind = "report";  // "report", "boot-kbd", "boot-mouse"
};

enum class BleState : uint8_t { Idle, Scanning, Connecting, Connected };

struct BleStatus {
  BleState state = BleState::Idle;
  bool hasTarget = false;
  bool autoConnect = true;
  std::string targetAddr;
  uint8_t targetType = 0;
  std::string targetName;
  int battery = -1;
  std::vector<BleReportSource> sources;
  std::string reportMapHex;
  std::vector<BleScanEntry> scan;
  std::string lastError;
};

const char* bleStateName(BleState s);

class BleHidHost {
 public:
  using ReportFn = std::function<void(const HidReport&)>;
  using LogFn = std::function<void(const char* kind, const std::string& msg)>;
  using LinkFn = std::function<void(bool connected)>;

  void begin(ReportFn onReport, LogFn log, LinkFn onLink);

  void requestScan();
  void requestConnect(const std::string& addr, uint8_t addrType, const std::string& name);
  void requestDisconnect();  // also pauses auto-reconnect until the next connect
  void requestForget();      // disconnect, delete the bond and the saved device

  BleStatus status();
  uint32_t revision() const { return revision_; }

  // Called from NimBLE callbacks. Not part of the public API.
  void onScanResult(const BleScanEntry& e);
  void onLinkLost(int reason);
  void onBattery(uint8_t level);

 private:
  enum class Cmd : uint8_t { Scan, Connect, Disconnect, Forget };
  struct Request {
    Cmd cmd;
    uint8_t addrType;
    char addr[18];
    char name[32];
  };

  static void taskEntry(void* arg);
  void run();
  void handle(const Request& r);
  void doScan();
  void doConnect();
  bool setupHid();
  void dropLink(const char* why);
  void saveTarget();
  void loadTarget();
  void setState(BleState s);
  void setError(const std::string& e);
  void bump() { revision_++; }

  ReportFn onReport_;
  LogFn log_;
  LinkFn onLink_;
  QueueHandle_t queue_ = nullptr;
  std::mutex mu_;  // guards status_
  BleStatus status_;
  std::atomic<uint32_t> revision_{1};
  std::atomic<bool> linkLost_{false};
  std::atomic<int> linkLostReason_{0};
  uint32_t nextAttemptMs_ = 0;
  uint32_t failedAttempts_ = 0;
  void* client_ = nullptr;  // NimBLEClient*, kept opaque to keep NimBLE out of this header
};

extern BleHidHost bleHost;
