// HID report capture and masked byte patterns.
//
// We deliberately do not parse HID report descriptors. Every input report the
// ring sends is treated as an opaque byte string tagged with the GATT handle it
// arrived on. Mappings match reports with byte patterns where some bytes may be
// wildcards ("??"), which copes with keyboard, consumer-control, mouse and
// touch/digitizer style reports alike.
//
// Pure C++ (no Arduino dependencies) so it can be unit tested on the host.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

constexpr size_t kMaxReportLen = 32;

struct HidReport {
  uint16_t src = 0;  // GATT characteristic value handle the report arrived on
  uint8_t len = 0;
  uint8_t data[kMaxReportLen] = {};

  static HidReport make(uint16_t src, const uint8_t* bytes, size_t n);
  // An all-zero report: "nothing pressed / finger lifted" for nearly every
  // keyboard, consumer-control and mouse style HID device.
  bool isIdle() const;
  std::string toString() const;  // "002a:01 00 3c"
};

struct Pattern {
  uint16_t src = 0;
  uint8_t len = 0;  // 0 = unset
  uint8_t data[kMaxReportLen] = {};
  uint8_t mask[kMaxReportLen] = {};  // bits set in mask must equal data

  bool empty() const { return len == 0; }
  bool matches(const HidReport& r) const;
  // True if some report could match both patterns.
  bool overlaps(const Pattern& other) const;
  // True if an all-zero report on this pattern's source would match.
  bool matchesIdle() const;
  int fixedBytes() const;

  static Pattern exact(const HidReport& r);
  // Bytes that agree across every report stay fixed, the rest become
  // wildcards. Reports on a different source than the first are ignored.
  static Pattern consensus(const std::vector<const HidReport*>& reports);

  // Text form: "<src hex>:<byte> <byte> ...", wildcard bytes written "??".
  // e.g. "002a:01 ?? 3c". The empty pattern is "".
  std::string toString() const;
  static bool parse(const std::string& text, Pattern& out);
};
