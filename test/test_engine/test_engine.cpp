// Host-side tests for the pattern matcher and learning engine.
//   pio test -e native
#include <unity.h>

#include <initializer_list>
#include <string>
#include <vector>

#include "engine.h"
#include "pattern.h"

namespace {

struct Msg {
  uint8_t s, d1, d2;
};

struct Harness {
  Engine e;
  std::vector<Msg> midi;
  uint32_t now = 1000;

  Harness() {
    e.midiOut = [this](uint8_t s, uint8_t a, uint8_t b) { midi.push_back({s, a, b}); };
  }

  void report(uint16_t src, std::initializer_list<uint8_t> bytes, uint32_t advanceMs = 10) {
    std::vector<uint8_t> v(bytes);
    e.onReport(HidReport::make(src, v.data(), v.size()), now);
    advance(advanceMs);
  }

  // Advance time, ticking every ms like the firmware's engine task.
  void advance(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
      now++;
      e.tick(now);
    }
  }

  uint32_t add(const char* name, uint8_t note) {
    Mapping m;
    m.name = name;
    m.number = note;
    return e.addMapping(m);
  }

  int countOn(uint8_t note) const {
    int n = 0;
    for (auto& m : midi) n += ((m.s & 0xF0) == 0x90 && m.d1 == note) ? 1 : 0;
    return n;
  }
  int countOff(uint8_t note) const {
    int n = 0;
    for (auto& m : midi) n += ((m.s & 0xF0) == 0x80 && m.d1 == note) ? 1 : 0;
    return n;
  }
};

// A key on a keyboard-style report: [mods, 0, key, 0, 0, 0, 0, 0] then zeros.
void keyTap(Harness& h, uint8_t key, uint32_t holdMs = 60) {
  h.report(0x2a, {0, 0, key, 0, 0, 0, 0, 0}, holdMs);
  h.report(0x2a, {0, 0, 0, 0, 0, 0, 0, 0}, 10);
}

// Emulated touch swipe: [tip, contact, xlo, xhi, ylo, yhi] reports.
void swipe(Harness& h, int dy) {
  int y = 0x80;
  h.report(0x30, {1, 1, 0x40, 0x01, (uint8_t)y, 0x02});
  for (int i = 0; i < 4; i++) {
    y += dy;
    h.report(0x30, {1, 1, 0x40, 0x01, (uint8_t)y, 0x02});
  }
  h.report(0x30, {0, 1, 0x40, 0x01, (uint8_t)y, 0x02});
}

void learnWith(Harness& h, uint32_t id, void (*press)(Harness&)) {
  TEST_ASSERT_TRUE(h.e.startLearn(id));
  for (int i = 0; i < h.e.settings.learnSamples; i++) {
    press(h);
    h.advance(600);
  }
  h.advance(2000);
  TEST_ASSERT_FALSE(h.e.learn().active);
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_pattern_roundtrip() {
  Pattern p;
  TEST_ASSERT_TRUE(Pattern::parse("002a:01 ?? 3C", p));
  TEST_ASSERT_EQUAL_STRING("002a:01 ?? 3c", p.toString().c_str());
  uint8_t a[] = {1, 0x99, 0x3c, 0x55};
  uint8_t b[] = {2, 0x99, 0x3c};
  TEST_ASSERT_TRUE(p.matches(HidReport::make(0x2a, a, 4)));
  TEST_ASSERT_FALSE(p.matches(HidReport::make(0x2a, b, 3)));
  TEST_ASSERT_FALSE(p.matches(HidReport::make(0x2b, a, 4)));
  TEST_ASSERT_TRUE(Pattern::parse("", p));
  TEST_ASSERT_TRUE(p.empty());
  TEST_ASSERT_FALSE(Pattern::parse("zz:01", p));
  TEST_ASSERT_FALSE(Pattern::parse("002a:0", p));
}

void test_consensus_wildcards_varying_bytes() {
  uint8_t a[] = {1, 7, 3};
  uint8_t b[] = {1, 8, 3};
  HidReport ra = HidReport::make(5, a, 3), rb = HidReport::make(5, b, 3);
  Pattern p = Pattern::consensus({&ra, &rb});
  TEST_ASSERT_EQUAL_STRING("0005:01 ?? 03", p.toString().c_str());
}

void test_keyboard_learn_and_play() {
  Harness h;
  uint32_t id = h.add("A", 60);
  learnWith(h, id, [](Harness& hh) { keyTap(hh, 0x04); });
  const Mapping* m = h.e.find(id);
  TEST_ASSERT_EQUAL(0, m->triggerIndex);
  TEST_ASSERT_EQUAL(static_cast<int>(ReleaseMode::Report), static_cast<int>(m->releaseMode));
  TEST_ASSERT_TRUE(h.midi.empty());  // nothing sent while learning

  h.report(0x2a, {0, 0, 4, 0, 0, 0, 0, 0}, 1000);  // held for a second
  TEST_ASSERT_EQUAL(1, h.countOn(60));
  TEST_ASSERT_EQUAL(0, h.countOff(60));  // still held, despite the gesture gap
  h.report(0x2a, {0, 0, 0, 0, 0, 0, 0, 0});
  TEST_ASSERT_EQUAL(1, h.countOff(60));

  keyTap(h, 0x05);  // different key: nothing
  TEST_ASSERT_EQUAL(1, h.countOn(60));
  keyTap(h, 0x04);
  TEST_ASSERT_EQUAL(2, h.countOn(60));
  TEST_ASSERT_EQUAL(2, h.countOff(60));
}

void test_learn_long_hold_still_finds_release() {
  Harness h;
  uint32_t id = h.add("held", 61);
  learnWith(h, id, [](Harness& hh) { keyTap(hh, 0x07, 900); });  // longer than learnGapMs
  const Mapping* m = h.e.find(id);
  TEST_ASSERT_EQUAL(static_cast<int>(ReleaseMode::Report), static_cast<int>(m->releaseMode));
  TEST_ASSERT_EQUAL_STRING("002a:00 00 00 00 00 00 00 00", m->release.toString().c_str());
}

void test_swipes_sharing_touch_down_are_disambiguated() {
  Harness h;
  uint32_t up = h.add("up", 62);
  uint32_t down = h.add("down", 63);
  learnWith(h, up, [](Harness& hh) { swipe(hh, -0x10); });
  TEST_ASSERT_EQUAL(0, h.e.find(up)->triggerIndex);
  learnWith(h, down, [](Harness& hh) { swipe(hh, 0x10); });

  // Both now trigger on the first report after the shared touch-down.
  TEST_ASSERT_EQUAL(1, h.e.find(down)->triggerIndex);
  TEST_ASSERT_EQUAL(1, h.e.find(up)->triggerIndex);
  TEST_ASSERT_TRUE(h.e.learn().notes.size() >= 2);  // summary + "moved up's trigger"

  swipe(h, 0x10);
  h.advance(300);
  TEST_ASSERT_EQUAL(1, h.countOn(63));
  TEST_ASSERT_EQUAL(1, h.countOff(63));
  TEST_ASSERT_EQUAL(0, h.countOn(62));

  swipe(h, -0x10);
  h.advance(300);
  TEST_ASSERT_EQUAL(1, h.countOn(62));
  TEST_ASSERT_EQUAL(1, h.countOff(62));
  TEST_ASSERT_EQUAL(1, h.countOn(63));
}

void test_single_report_press_releases_on_gesture_end() {
  Harness h;
  uint32_t id = h.add("single", 64);
  learnWith(h, id, [](Harness& hh) { hh.report(0x40, {0xe9, 0x00}); });
  TEST_ASSERT_EQUAL(static_cast<int>(ReleaseMode::GestureEnd), static_cast<int>(h.e.find(id)->releaseMode));
  h.report(0x40, {0xe9, 0x00}, 50);
  TEST_ASSERT_EQUAL(1, h.countOn(64));
  TEST_ASSERT_EQUAL(0, h.countOff(64));
  h.advance(200);
  TEST_ASSERT_EQUAL(1, h.countOff(64));
}

void test_counter_byte_is_wildcarded() {
  Harness h;
  uint32_t id = h.add("ctr", 65);
  static uint8_t counter = 0;
  learnWith(h, id, [](Harness& hh) {
    counter++;
    hh.report(0x50, {0x02, counter});
    hh.report(0x50, {0x00, 0x00});
  });
  h.report(0x50, {0x02, 0x77});
  TEST_ASSERT_EQUAL(1, h.countOn(65));
}

void test_missed_release_retriggers() {
  Harness h;
  uint32_t id = h.add("A", 66);
  learnWith(h, id, [](Harness& hh) { keyTap(hh, 0x04); });
  h.report(0x2a, {0, 0, 4, 0, 0, 0, 0, 0}, 300);
  h.report(0x2a, {0, 0, 4, 0, 0, 0, 0, 0}, 300);  // release was lost
  TEST_ASSERT_EQUAL(2, h.countOn(66));
  TEST_ASSERT_EQUAL(1, h.countOff(66));
}

void test_timed_and_disabled() {
  Harness h;
  uint32_t id = h.add("A", 67);
  learnWith(h, id, [](Harness& hh) { keyTap(hh, 0x04); });
  Mapping m = *h.e.find(id);
  m.releaseMode = ReleaseMode::Timed;
  m.holdMs = 50;
  h.e.updateMapping(m);
  h.report(0x2a, {0, 0, 4, 0, 0, 0, 0, 0}, 1000);
  TEST_ASSERT_EQUAL(1, h.countOn(67));
  TEST_ASSERT_EQUAL(1, h.countOff(67));
  m.enabled = false;
  h.e.updateMapping(m);
  keyTap(h, 0x04);
  TEST_ASSERT_EQUAL(1, h.countOn(67));
}

void test_identical_buttons_warn_but_both_fire() {
  Harness h;
  uint32_t a = h.add("a", 70), b = h.add("b", 71);
  learnWith(h, a, [](Harness& hh) { keyTap(hh, 0x04); });
  learnWith(h, b, [](Harness& hh) { keyTap(hh, 0x04); });
  bool warned = false;
  for (auto& n : h.e.learn().notes) warned = warned || n.find("apart") != std::string::npos;
  TEST_ASSERT_TRUE(warned);
  keyTap(h, 0x04);
  TEST_ASSERT_EQUAL(1, h.countOn(70));
  TEST_ASSERT_EQUAL(1, h.countOn(71));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_pattern_roundtrip);
  RUN_TEST(test_consensus_wildcards_varying_bytes);
  RUN_TEST(test_keyboard_learn_and_play);
  RUN_TEST(test_learn_long_hold_still_finds_release);
  RUN_TEST(test_swipes_sharing_touch_down_are_disambiguated);
  RUN_TEST(test_single_report_press_releases_on_gesture_end);
  RUN_TEST(test_counter_byte_is_wildcarded);
  RUN_TEST(test_missed_release_retriggers);
  RUN_TEST(test_timed_and_disabled);
  RUN_TEST(test_identical_buttons_warn_but_both_fire);
  return UNITY_END();
}
