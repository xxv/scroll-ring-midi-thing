#include "ble_hid_host.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

#include <algorithm>

#include "app_config.h"

BleHidHost bleHost;

namespace {

const NimBLEUUID kHidService(static_cast<uint16_t>(0x1812));
const NimBLEUUID kReport(static_cast<uint16_t>(0x2A4D));
const NimBLEUUID kReportMap(static_cast<uint16_t>(0x2A4B));
const NimBLEUUID kProtocolMode(static_cast<uint16_t>(0x2A4E));
const NimBLEUUID kBootKeyboardIn(static_cast<uint16_t>(0x2A22));
const NimBLEUUID kBootMouseIn(static_cast<uint16_t>(0x2A33));
const NimBLEUUID kReportReference(static_cast<uint16_t>(0x2908));
const NimBLEUUID kBatteryService(static_cast<uint16_t>(0x180F));
const NimBLEUUID kBatteryLevel(static_cast<uint16_t>(0x2A19));

constexpr uint32_t kScanMs = 10000;
constexpr uint32_t kConnectTimeoutMs = 8000;
constexpr size_t kMaxReportMap = 512;
constexpr size_t kMaxScanEntries = 40;

// Scan list ranking: HID devices first, then strongest signal.
bool ranksBelow(const BleScanEntry& a, const BleScanEntry& b) {
  if (a.hid != b.hid) return !a.hid;
  return a.rssi < b.rssi;
}

std::string hexBytes(const uint8_t* d, size_t n) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  s.reserve(n * 3);
  for (size_t i = 0; i < n; i++) {
    if (i) s += ' ';
    s += digits[d[i] >> 4];
    s += digits[d[i] & 0xF];
  }
  return s;
}

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* d) override {
    BleScanEntry e;
    e.addr = d->getAddress().toString();
    e.addrType = d->getAddress().getType();
    e.name = d->getName();
    e.rssi = d->getRSSI();
    if (d->haveAppearance()) e.appearance = d->getAppearance();
    // Appearance category 0x0F (values 0x03C0-0x03FF) is "Human Interface Device".
    e.hid = d->isAdvertisingService(kHidService) || (e.appearance >> 6) == 0x0F;
    bleHost.onScanResult(e);
  }
};

class ClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient*, int reason) override { bleHost.onLinkLost(reason); }
  // Rings have no display or keypad; accept "just works" style pairing.
  void onConfirmPasskey(NimBLEConnInfo& info, uint32_t) override { NimBLEDevice::injectConfirmPasskey(info, true); }
  void onPassKeyEntry(NimBLEConnInfo& info) override { NimBLEDevice::injectPassKey(info, 0); }
};

ScanCallbacks scanCallbacks;
ClientCallbacks clientCallbacks;

NimBLEClient* asClient(void* p) { return static_cast<NimBLEClient*>(p); }

}  // namespace

const char* bleStateName(BleState s) {
  switch (s) {
    case BleState::Idle: return "idle";
    case BleState::Scanning: return "scanning";
    case BleState::Connecting: return "connecting";
    case BleState::Connected: return "connected";
  }
  return "idle";
}

void BleHidHost::begin(ReportFn onReport, LogFn log, LinkFn onLink) {
  onReport_ = std::move(onReport);
  log_ = std::move(log);
  onLink_ = std::move(onLink);

  NimBLEDevice::init(BLE_DEVICE_NAME);
  NimBLEDevice::setSecurityAuth(true /*bond*/, false /*mitm*/, true /*secure connections*/);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setPower(9);  // dBm; the ring is usually close

  loadTarget();
  queue_ = xQueueCreate(8, sizeof(Request));
  xTaskCreatePinnedToCore(taskEntry, "ble_host", 8192, this, 2, nullptr, 0);
}

void BleHidHost::taskEntry(void* arg) { static_cast<BleHidHost*>(arg)->run(); }

// --- Requests (any task) ---------------------------------------------------

namespace {
void cancelPendingConnect(void* client, BleState state) {
  if (client && state == BleState::Connecting) asClient(client)->cancelConnect();
}
}  // namespace

void BleHidHost::requestScan() {
  Request r{Cmd::Scan, 0, {}, {}};
  xQueueSend(queue_, &r, 0);
  cancelPendingConnect(client_, status_.state);
}

void BleHidHost::requestConnect(const std::string& addr, uint8_t addrType, const std::string& name) {
  Request r{Cmd::Connect, addrType, {}, {}};
  strlcpy(r.addr, addr.c_str(), sizeof(r.addr));
  strlcpy(r.name, name.c_str(), sizeof(r.name));
  xQueueSend(queue_, &r, 0);
  cancelPendingConnect(client_, status_.state);
}

void BleHidHost::requestDisconnect() {
  Request r{Cmd::Disconnect, 0, {}, {}};
  xQueueSend(queue_, &r, 0);
  cancelPendingConnect(client_, status_.state);
}

void BleHidHost::requestForget() {
  Request r{Cmd::Forget, 0, {}, {}};
  xQueueSend(queue_, &r, 0);
  cancelPendingConnect(client_, status_.state);
}

BleStatus BleHidHost::status() {
  std::lock_guard<std::mutex> lock(mu_);
  BleStatus s = status_;
  std::sort(s.scan.begin(), s.scan.end(), [](const BleScanEntry& a, const BleScanEntry& b) { return ranksBelow(b, a); });
  return s;
}

// --- Callbacks (NimBLE host task) ------------------------------------------

void BleHidHost::onScanResult(const BleScanEntry& e) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = std::find_if(status_.scan.begin(), status_.scan.end(),
                           [&](const BleScanEntry& x) { return x.addr == e.addr; });
    if (it != status_.scan.end()) {
      std::string name = it->name;
      *it = e;
      if (it->name.empty()) it->name = name;
    } else if (status_.scan.size() < kMaxScanEntries) {
      status_.scan.push_back(e);
    } else {
      // Crowded room: keep the best kMaxScanEntries, evicting the weakest.
      auto weakest = std::min_element(status_.scan.begin(), status_.scan.end(), ranksBelow);
      if (ranksBelow(*weakest, e)) *weakest = e;
    }
  }
  bump();
}

void BleHidHost::onLinkLost(int reason) {
  linkLostReason_ = reason;
  linkLost_ = true;
}

void BleHidHost::onBattery(uint8_t level) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    status_.battery = level;
  }
  bump();
}

// --- BLE task ---------------------------------------------------------------

void BleHidHost::setState(BleState s) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    status_.state = s;
  }
  bump();
}

void BleHidHost::setError(const std::string& e) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    status_.lastError = e;
  }
  bump();
  if (!e.empty()) log_("warn", e);
}

void BleHidHost::run() {
  for (;;) {
    Request r;
    if (xQueueReceive(queue_, &r, pdMS_TO_TICKS(100)) == pdTRUE) handle(r);

    if (linkLost_) {
      linkLost_ = false;
      if (status_.state == BleState::Connected) dropLink("link lost");
    }

    bool wantConnect;
    {
      std::lock_guard<std::mutex> lock(mu_);
      wantConnect = status_.state == BleState::Idle && status_.hasTarget && status_.autoConnect;
    }
    if (wantConnect && static_cast<int32_t>(millis() - nextAttemptMs_) >= 0) doConnect();
  }
}

void BleHidHost::handle(const Request& r) {
  NimBLEClient* c = asClient(client_);
  switch (r.cmd) {
    case Cmd::Scan:
      doScan();
      break;

    case Cmd::Connect: {
      if (c && c->isConnected()) {
        c->disconnect();
        dropLink("switching device");
      }
      {
        std::lock_guard<std::mutex> lock(mu_);
        status_.hasTarget = true;
        status_.autoConnect = true;
        status_.targetAddr = r.addr;
        status_.targetType = r.addrType;
        status_.targetName = r.name;
        status_.lastError.clear();
      }
      bump();
      failedAttempts_ = 0;
      nextAttemptMs_ = millis();
      break;
    }

    case Cmd::Disconnect:
      {
        std::lock_guard<std::mutex> lock(mu_);
        status_.autoConnect = false;
      }
      if (c && c->isConnected()) {
        c->disconnect();
        dropLink("disconnected by user");
      } else {
        setState(BleState::Idle);
      }
      break;

    case Cmd::Forget: {
      if (c && c->isConnected()) {
        c->disconnect();
        dropLink("forgetting device");
      }
      std::string addr;
      uint8_t type;
      {
        std::lock_guard<std::mutex> lock(mu_);
        addr = status_.targetAddr;
        type = status_.targetType;
        status_.hasTarget = false;
        status_.targetAddr.clear();
        status_.targetName.clear();
        status_.reportMapHex.clear();
        status_.state = BleState::Idle;
      }
      if (!addr.empty()) NimBLEDevice::deleteBond(NimBLEAddress(addr, type));
      Preferences p;
      p.begin("ring", false);
      p.clear();
      p.end();
      bump();
      log_("ble", "Forgot device " + addr);
      break;
    }
  }
}

void BleHidHost::doScan() {
  NimBLEClient* c = asClient(client_);
  const bool connected = c && c->isConnected();
  {
    std::lock_guard<std::mutex> lock(mu_);
    status_.scan.clear();
  }
  setState(BleState::Scanning);
  log_("ble", "Scanning for 10 s. Put the ring in pairing mode (and unpair it from your phone).");

  NimBLEScan* s = NimBLEDevice::getScan();
  s->setScanCallbacks(&scanCallbacks, false);
  s->setActiveScan(true);
  s->setInterval(100);
  s->setWindow(100);
  s->setMaxResults(0);  // results arrive via callbacks
  s->start(kScanMs, false, true);
  uint32_t start = millis();
  while (s->isScanning() && millis() - start < kScanMs + 1000) {
    // Stop early if the user picked a device.
    if (uxQueueMessagesWaiting(queue_)) break;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  s->stop();
  setState(connected && c->isConnected() ? BleState::Connected : BleState::Idle);
  log_("ble", "Scan finished: " + std::to_string(status().scan.size()) + " devices");
}

void BleHidHost::doConnect() {
  NimBLEClient* c = asClient(client_);
  if (!c) {
    c = NimBLEDevice::createClient();
    c->setClientCallbacks(&clientCallbacks, false);
    c->setConnectTimeout(kConnectTimeoutMs);
    client_ = c;
  }

  std::string addr, name;
  uint8_t type;
  {
    std::lock_guard<std::mutex> lock(mu_);
    addr = status_.targetAddr;
    type = status_.targetType;
    name = status_.targetName.empty() ? addr : status_.targetName;
  }
  setState(BleState::Connecting);
  if (failedAttempts_ == 0) log_("ble", "Connecting to " + name + "... (press a ring button to wake it)");

  linkLost_ = false;
  if (!c->connect(NimBLEAddress(addr, type), true)) {
    failedAttempts_++;
    setState(BleState::Idle);
    // Keep listening: a sleeping ring only advertises briefly after a press.
    nextAttemptMs_ = millis() + (failedAttempts_ < 5 ? 100 : 1000);
    return;
  }

  log_("ble", "Link up, setting up HID...");
  if (!setupHid()) {
    c->disconnect();
    failedAttempts_++;
    setState(BleState::Idle);
    nextAttemptMs_ = millis() + 3000;
    return;
  }

  failedAttempts_ = 0;
  setError("");
  setState(BleState::Connected);
  saveTarget();
  log_("ble", "Connected to " + name);
  onLink_(true);
}

bool BleHidHost::setupHid() {
  NimBLEClient* c = asClient(client_);

  // Pair on first connection, or re-encrypt with the stored bond. Some rings
  // start security themselves, so a failure here is not necessarily fatal.
  if (!c->secureConnection()) log_("warn", "Encryption/pairing did not complete; trying anyway");

  NimBLERemoteService* hid = c->getService(kHidService);
  if (!hid) {
    setError("Device has no HID service (is it a classic-Bluetooth-only device?)");
    return false;
  }

  if (NimBLERemoteCharacteristic* pm = hid->getCharacteristic(kProtocolMode)) {
    uint8_t reportProtocol = 1;
    pm->writeValue(&reportProtocol, 1, false);
  }

  std::string reportMap;
  if (NimBLERemoteCharacteristic* rm = hid->getCharacteristic(kReportMap)) {
    NimBLEAttValue v = rm->readValue();
    reportMap = hexBytes(v.data(), std::min<size_t>(v.size(), kMaxReportMap));
  }

  auto forward = [this](NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool) {
    onReport_(HidReport::make(chr->getHandle(), data, len));
  };

  std::vector<BleReportSource> sources;
  for (NimBLERemoteCharacteristic* chr : hid->getCharacteristics(true)) {
    if (chr->getUUID() != kReport || !(chr->canNotify() || chr->canIndicate())) continue;
    BleReportSource src;
    src.handle = chr->getHandle();
    if (NimBLERemoteDescriptor* ref = chr->getDescriptor(kReportReference)) {
      NimBLEAttValue v = ref->readValue();
      if (v.size() >= 2) {
        src.reportId = v.data()[0];
        src.reportType = v.data()[1];
      }
    }
    if (src.reportType > 1) continue;  // output/feature reports
    if (chr->subscribe(chr->canNotify(), forward)) {
      sources.push_back(src);
    } else {
      log_("warn", "Could not subscribe to report handle " + std::to_string(src.handle));
    }
  }

  // Devices that only expose boot-protocol reports.
  if (sources.empty()) {
    for (const auto& [uuid, kind] : {std::pair<NimBLEUUID, const char*>{kBootKeyboardIn, "boot-kbd"},
                                     std::pair<NimBLEUUID, const char*>{kBootMouseIn, "boot-mouse"}}) {
      NimBLERemoteCharacteristic* chr = hid->getCharacteristic(uuid);
      if (chr && chr->canNotify() && chr->subscribe(true, forward)) {
        BleReportSource src;
        src.handle = chr->getHandle();
        src.kind = kind;
        sources.push_back(src);
      }
    }
  }

  if (sources.empty()) {
    setError("HID service has no input reports we could subscribe to");
    return false;
  }

  int battery = -1;
  if (NimBLERemoteService* bs = c->getService(kBatteryService)) {
    if (NimBLERemoteCharacteristic* bl = bs->getCharacteristic(kBatteryLevel)) {
      NimBLEAttValue v = bl->readValue();
      if (v.size() >= 1) battery = v.data()[0];
      if (bl->canNotify()) {
        bl->subscribe(true, [this](NimBLERemoteCharacteristic*, uint8_t* d, size_t n, bool) {
          if (n >= 1) onBattery(d[0]);
        });
      }
    }
  }

  std::string summary;
  for (const auto& s : sources) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%s%04x(id %u)", summary.empty() ? "" : ", ", s.handle, s.reportId);
    summary += buf;
  }
  log_("ble", "Subscribed to " + std::to_string(sources.size()) + " input report(s): " + summary);

  {
    std::lock_guard<std::mutex> lock(mu_);
    status_.sources = sources;
    status_.reportMapHex = reportMap;
    status_.battery = battery;
  }
  bump();
  return true;
}

void BleHidHost::dropLink(const char* why) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    status_.state = BleState::Idle;
    status_.sources.clear();
    status_.battery = -1;
  }
  bump();
  log_("ble", std::string("Disconnected (") + why + ", reason " + std::to_string(linkLostReason_) + ")");
  nextAttemptMs_ = millis() + 500;
  onLink_(false);
}

void BleHidHost::saveTarget() {
  std::string addr, name;
  uint8_t type;
  {
    std::lock_guard<std::mutex> lock(mu_);
    addr = status_.targetAddr;
    name = status_.targetName;
    type = status_.targetType;
  }
  Preferences p;
  p.begin("ring", false);
  if (p.getString("addr", "") != String(addr.c_str()) || p.getUChar("type", 255) != type ||
      p.getString("name", "") != String(name.c_str())) {
    p.putString("addr", addr.c_str());
    p.putUChar("type", type);
    p.putString("name", name.c_str());
  }
  p.end();
}

void BleHidHost::loadTarget() {
  Preferences p;
  if (!p.begin("ring", true)) return;  // namespace doesn't exist yet
  String addr = p.getString("addr", "");
  if (addr.length()) {
    std::lock_guard<std::mutex> lock(mu_);
    status_.hasTarget = true;
    status_.targetAddr = addr.c_str();
    status_.targetType = p.getUChar("type", 0);
    status_.targetName = p.getString("name", "").c_str();
  }
  p.end();
}
