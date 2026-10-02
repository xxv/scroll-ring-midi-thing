// Mapping engine: turns a stream of raw HID reports into MIDI messages, and
// learns new mappings by watching what the ring sends.
//
// Gestures
//   Reports are grouped into "gestures": a gesture starts at the first
//   non-idle report and ends at an idle (all-zero) report or after
//   Settings::gestureGapMs of silence. A key press is typically
//   [down, up(zeros)]; an emulated touch swipe is [touch-down, move...,
//   lift] followed by silence.
//
// Learning
//   The user presses the button a few times (Settings::learnSamples). Each
//   press is captured as a sample. The samples are combined index by index into
//   a "consensus gesture" in which bytes that differ between presses become
//   wildcards.
//   - The trigger is the earliest report of the gesture that cannot also
//     appear in another mapping's gesture. Two buttons that emulate swipes
//     starting at the same point therefore trigger on the first report where
//     they diverge, not on the shared touch-down.
//   - The release is the gesture's final report (key up / finger lift) when
//     that report never shows up earlier in the gesture; otherwise the note
//     ends when the gesture ends.
//
// Pure C++ (no Arduino dependencies) so it can be unit tested on the host.
// Not thread-safe: the caller serialises access.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "pattern.h"

constexpr size_t kMaxMappings = 64;
constexpr size_t kMaxGestureLen = 12;    // consensus reports kept per mapping
constexpr size_t kMaxSampleReports = 48; // reports captured per learn sample
constexpr int kMaxLearnSamples = 5;

enum class MidiAction : uint8_t {
  Note = 0,
  // Planned: Cc (momentary on/off value), CcStep (relative scroll), ...
};

enum class ReleaseMode : uint8_t {
  Report = 0,      // note off when the learned release report arrives
  GestureEnd = 1,  // note off when the gesture ends (idle report or gap)
  Timed = 2,       // note off holdMs after note on
};

const char* releaseModeName(ReleaseMode m);
bool parseReleaseMode(const std::string& s, ReleaseMode& out);
const char* midiActionName(MidiAction a);
bool parseMidiAction(const std::string& s, MidiAction& out);

struct Mapping {
  uint32_t id = 0;
  std::string name;
  bool enabled = true;

  MidiAction action = MidiAction::Note;
  uint8_t channel = 1;  // 1..16
  uint8_t number = 60;  // note number (CC number later)
  uint8_t velocity = 100;

  Pattern trigger;
  Pattern release;
  ReleaseMode releaseMode = ReleaseMode::GestureEnd;
  uint16_t holdMs = 100;

  std::vector<Pattern> gesture;  // learned consensus gesture
  int triggerIndex = -1;         // index into gesture, -1 = hand-edited
  bool triggerAuto = true;       // engine may move the trigger to avoid conflicts

  // Runtime state, not persisted.
  bool active = false;
  bool firedThisGesture = false;
  uint32_t offAt = 0;  // when non-zero, note off at this time
  uint8_t sentChannel = 0, sentNumber = 0;  // what the active note-on used
};

struct Settings {
  uint16_t gestureGapMs = 150;  // silence that ends a gesture at runtime
  uint16_t learnGapMs = 400;    // silence that ends a learn sample
  uint16_t learnTailMs = 1500;  // a key held longer than learnGapMs can still
                                // contribute its release report within this
  uint8_t learnSamples = 3;     // presses captured per learn
};

struct LearnState {
  bool active = false;
  uint32_t mappingId = 0;
  std::vector<std::vector<HidReport>> samples;
  std::vector<HidReport> current;
  bool capturing = false;
  uint32_t lastMs = 0;
  uint32_t tailUntil = 0;

  // Outcome of the most recent learn, for the UI.
  uint32_t resultMappingId = 0;
  std::vector<std::string> notes;
};

class Engine {
 public:
  // Raw 3-byte MIDI message out.
  std::function<void(uint8_t status, uint8_t d1, uint8_t d2)> midiOut;
  // Human-readable log line. kind: "info", "warn", "learn".
  std::function<void(const char* kind, const std::string& msg)> log;
  // A mapping's note went on or off.
  std::function<void(const Mapping& m, bool on)> noteSent;
  // Every report seen, with a summary of what it did ("" if nothing).
  std::function<void(const HidReport& r, const std::string& effect)> reportSeen;

  Settings settings;

  const std::vector<Mapping>& mappings() const { return mappings_; }
  Mapping* find(uint32_t id);

  // Config changes. Each bumps revision() and marks the config dirty.
  uint32_t addMapping(Mapping m);  // returns new id, 0 if full
  bool updateMapping(const Mapping& m);  // config fields only; keeps runtime state
  bool removeMapping(uint32_t id);
  bool pickTrigger(uint32_t id, int gestureIndex);
  void replaceAll(std::vector<Mapping> mappings, const Settings& s);
  void setSettings(const Settings& s);

  // Runtime.
  void onReport(const HidReport& r, uint32_t nowMs);
  void tick(uint32_t nowMs);
  void allNotesOff();
  bool testMapping(uint32_t id, uint32_t nowMs);

  // Learning.
  bool startLearn(uint32_t id);
  void finishLearn();
  void cancelLearn();
  const LearnState& learn() const { return learn_; }

  // Changes to anything the UI shows (mappings, learn progress).
  uint32_t revision() const { return revision_; }
  // Persistent config changed since last clearDirty().
  bool dirty() const { return dirty_; }
  void clearDirty() { dirty_ = false; }

  // Exposed for tests.
  static void deriveFromSamples(Mapping& m, const std::vector<std::vector<HidReport>>& samples);
  // Picks the earliest usable gesture report as trigger. Returns false (and
  // falls back to the earliest usable report) if every candidate could also
  // match one of `others`' gestures; conflictName then names one of them.
  static bool chooseTrigger(Mapping& m, const std::vector<Mapping>& others, std::string* conflictName = nullptr);

 private:
  void noteOn(Mapping& m);
  void noteOff(Mapping& m);
  void endGesture();
  void learnReport(const HidReport& r, uint32_t nowMs);
  void learnTick(uint32_t nowMs);
  void changed(bool config);
  void logf(const char* kind, const std::string& msg);
  std::vector<Mapping> othersThan(uint32_t id) const;

  std::vector<Mapping> mappings_;
  LearnState learn_;
  uint32_t nextId_ = 1;
  bool gestureActive_ = false;
  uint32_t lastReportMs_ = 0;
  uint32_t revision_ = 1;
  bool dirty_ = false;
};
