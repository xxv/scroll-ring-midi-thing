#include "pattern.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace {

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

void appendHex(std::string& s, unsigned v, int digits) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%0*x", digits, v);
  s += buf;
}

}  // namespace

HidReport HidReport::make(uint16_t src, const uint8_t* bytes, size_t n) {
  HidReport r;
  r.src = src;
  r.len = static_cast<uint8_t>(std::min(n, kMaxReportLen));
  std::copy(bytes, bytes + r.len, r.data);
  return r;
}

bool HidReport::isIdle() const {
  return std::all_of(data, data + len, [](uint8_t b) { return b == 0; });
}

std::string HidReport::toString() const {
  std::string s;
  appendHex(s, src, 4);
  s += ':';
  for (uint8_t i = 0; i < len; i++) {
    if (i) s += ' ';
    appendHex(s, data[i], 2);
  }
  return s;
}

bool Pattern::matches(const HidReport& r) const {
  if (empty() || r.src != src || r.len < len) return false;
  for (uint8_t i = 0; i < len; i++) {
    if ((r.data[i] ^ data[i]) & mask[i]) return false;
  }
  return true;
}

bool Pattern::overlaps(const Pattern& o) const {
  if (empty() || o.empty() || src != o.src) return false;
  uint8_t n = std::min(len, o.len);
  for (uint8_t i = 0; i < n; i++) {
    if ((data[i] ^ o.data[i]) & mask[i] & o.mask[i]) return false;
  }
  return true;
}

bool Pattern::matchesIdle() const {
  if (empty()) return false;
  for (uint8_t i = 0; i < len; i++) {
    if (data[i] & mask[i]) return false;
  }
  return true;
}

int Pattern::fixedBytes() const {
  int n = 0;
  for (uint8_t i = 0; i < len; i++) n += mask[i] ? 1 : 0;
  return n;
}

Pattern Pattern::exact(const HidReport& r) {
  Pattern p;
  p.src = r.src;
  p.len = r.len;
  std::copy(r.data, r.data + r.len, p.data);
  std::fill(p.mask, p.mask + r.len, 0xFF);
  return p;
}

Pattern Pattern::consensus(const std::vector<const HidReport*>& reports) {
  if (reports.empty()) return Pattern();
  Pattern p = exact(*reports.front());
  for (const HidReport* r : reports) {
    if (r->src != p.src) continue;
    p.len = std::min(p.len, r->len);
    for (uint8_t i = 0; i < p.len; i++) {
      // Byte-granular on purpose so every pattern has a text form.
      if (r->data[i] != p.data[i]) p.mask[i] = 0;
    }
  }
  for (uint8_t i = 0; i < p.len; i++) {
    if (!p.mask[i]) p.data[i] = 0;
  }
  return p;
}

std::string Pattern::toString() const {
  if (empty()) return "";
  std::string s;
  appendHex(s, src, 4);
  s += ':';
  for (uint8_t i = 0; i < len; i++) {
    if (i) s += ' ';
    if (mask[i] == 0xFF) {
      appendHex(s, data[i], 2);
    } else {
      s += "??";
    }
  }
  return s;
}

bool Pattern::parse(const std::string& text, Pattern& out) {
  out = Pattern();
  size_t start = text.find_first_not_of(" \t");
  if (start == std::string::npos) return true;  // empty pattern
  size_t colon = text.find(':', start);
  if (colon == std::string::npos) return false;

  unsigned src = 0;
  int srcDigits = 0;
  for (size_t i = start; i < colon; i++) {
    char c = text[i];
    if (c == ' ' || c == '\t') continue;
    if (srcDigits == 0 && c == '0' && i + 1 < colon && (text[i + 1] == 'x' || text[i + 1] == 'X')) {
      i++;
      continue;
    }
    int v = hexVal(c);
    if (v < 0 || ++srcDigits > 4) return false;
    src = src * 16 + v;
  }
  if (srcDigits == 0) return false;

  Pattern p;
  p.src = static_cast<uint16_t>(src);
  size_t i = colon + 1;
  while (i < text.size()) {
    char c = text[i];
    if (c == ' ' || c == '\t' || c == ',') {
      i++;
      continue;
    }
    if (i + 1 >= text.size() || p.len >= kMaxReportLen) return false;
    char c2 = text[i + 1];
    if ((c == '?' || c == 'x' || c == 'X') && (c2 == '?' || c2 == 'x' || c2 == 'X')) {
      p.data[p.len] = 0;
      p.mask[p.len] = 0;
    } else {
      int hi = hexVal(c), lo = hexVal(c2);
      if (hi < 0 || lo < 0) return false;
      p.data[p.len] = static_cast<uint8_t>(hi * 16 + lo);
      p.mask[p.len] = 0xFF;
    }
    p.len++;
    i += 2;
  }
  if (p.len == 0) return false;
  out = p;
  return true;
}
