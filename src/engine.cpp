#include "engine.h"

#include <algorithm>

namespace {

std::string mappingLabel(const Mapping& m) {
  return m.name.empty() ? "#" + std::to_string(m.id) : "\"" + m.name + "\"";
}

}  // namespace

const char* releaseModeName(ReleaseMode m) {
  switch (m) {
    case ReleaseMode::Report: return "report";
    case ReleaseMode::GestureEnd: return "gesture";
    case ReleaseMode::Timed: return "timed";
  }
  return "gesture";
}

bool parseReleaseMode(const std::string& s, ReleaseMode& out) {
  if (s == "report") out = ReleaseMode::Report;
  else if (s == "gesture") out = ReleaseMode::GestureEnd;
  else if (s == "timed") out = ReleaseMode::Timed;
  else return false;
  return true;
}

const char* midiActionName(MidiAction a) {
  switch (a) {
    case MidiAction::Note: return "note";
  }
  return "note";
}

bool parseMidiAction(const std::string& s, MidiAction& out) {
  if (s == "note") out = MidiAction::Note;
  else return false;
  return true;
}

Mapping* Engine::find(uint32_t id) {
  for (auto& m : mappings_) {
    if (m.id == id) return &m;
  }
  return nullptr;
}

std::vector<Mapping> Engine::othersThan(uint32_t id) const {
  std::vector<Mapping> out;
  for (const auto& m : mappings_) {
    if (m.id != id) out.push_back(m);
  }
  return out;
}

void Engine::changed(bool config) {
  revision_++;
  if (config) dirty_ = true;
}

void Engine::logf(const char* kind, const std::string& msg) {
  if (log) log(kind, msg);
}

// ---------------------------------------------------------------------------
// Config

uint32_t Engine::addMapping(Mapping m) {
  if (mappings_.size() >= kMaxMappings) return 0;
  m.id = nextId_++;
  m.active = false;
  m.firedThisGesture = false;
  m.offAt = 0;
  mappings_.push_back(m);
  changed(true);
  return m.id;
}

bool Engine::updateMapping(const Mapping& in) {
  Mapping* m = find(in.id);
  if (!m) return false;
  // Don't leave a note hanging if its channel/number is about to change.
  noteOff(*m);
  bool fired = m->firedThisGesture;
  *m = in;
  m->active = false;
  m->offAt = 0;
  m->firedThisGesture = fired;
  changed(true);
  return true;
}

bool Engine::removeMapping(uint32_t id) {
  if (learn_.active && learn_.mappingId == id) cancelLearn();
  for (auto it = mappings_.begin(); it != mappings_.end(); ++it) {
    if (it->id == id) {
      if (it->active) noteOff(*it);
      mappings_.erase(it);
      changed(true);
      return true;
    }
  }
  return false;
}

bool Engine::pickTrigger(uint32_t id, int index) {
  Mapping* m = find(id);
  if (!m || index < 0 || index >= static_cast<int>(m->gesture.size())) return false;
  m->triggerIndex = index;
  m->trigger = m->gesture[index];
  m->triggerAuto = false;
  changed(true);
  return true;
}

void Engine::replaceAll(std::vector<Mapping> mappings, const Settings& s) {
  allNotesOff();
  cancelLearn();
  mappings_ = std::move(mappings);
  if (mappings_.size() > kMaxMappings) mappings_.resize(kMaxMappings);
  nextId_ = 1;
  for (auto& m : mappings_) {
    if (find(m.id) != &m) m.id = 0;  // duplicate id: reassigned below
    nextId_ = std::max(nextId_, m.id + 1);
  }
  for (auto& m : mappings_) {
    if (m.id == 0) m.id = nextId_++;
    m.active = false;
    m.firedThisGesture = false;
    m.offAt = 0;
  }
  setSettings(s);
}

void Engine::setSettings(const Settings& s) {
  settings = s;
  settings.learnSamples = std::max<uint8_t>(1, std::min<uint8_t>(settings.learnSamples, kMaxLearnSamples));
  changed(true);
}

// ---------------------------------------------------------------------------
// Runtime

void Engine::noteOn(Mapping& m) {
  uint8_t ch = static_cast<uint8_t>((m.channel - 1) & 0x0F);
  switch (m.action) {
    case MidiAction::Note:
      if (midiOut) midiOut(0x90 | ch, m.number & 0x7F, m.velocity & 0x7F);
      break;
  }
  m.active = true;
  m.sentChannel = ch;
  m.sentNumber = m.number & 0x7F;
  if (noteSent) noteSent(m, true);
}

void Engine::noteOff(Mapping& m) {
  if (!m.active) return;
  switch (m.action) {
    case MidiAction::Note:
      if (midiOut) midiOut(0x80 | m.sentChannel, m.sentNumber, 0);
      break;
  }
  m.active = false;
  m.offAt = 0;
  if (noteSent) noteSent(m, false);
}

void Engine::allNotesOff() {
  for (auto& m : mappings_) noteOff(m);
}

bool Engine::testMapping(uint32_t id, uint32_t nowMs) {
  Mapping* m = find(id);
  if (!m) return false;
  noteOff(*m);
  noteOn(*m);
  m->offAt = (nowMs + 300) | 1;  // never 0
  return true;
}

void Engine::endGesture() {
  gestureActive_ = false;
  for (auto& m : mappings_) {
    if (m.active && m.releaseMode == ReleaseMode::GestureEnd) noteOff(m);
  }
}

void Engine::onReport(const HidReport& r, uint32_t nowMs) {
  if (learn_.active) {
    learnReport(r, nowMs);
    if (reportSeen) reportSeen(r, "learning");
    return;
  }

  const bool idle = r.isIdle();
  if (!gestureActive_ && !idle) {
    gestureActive_ = true;
    for (auto& m : mappings_) m.firedThisGesture = false;
  }
  lastReportMs_ = nowMs;

  std::string effect;
  for (auto& m : mappings_) {
    if (!m.enabled) continue;
    if (m.active && m.releaseMode == ReleaseMode::Report && m.release.matches(r)) {
      noteOff(m);
      // Re-arm so a quick second press inside the same gesture window works.
      m.firedThisGesture = false;
      effect += (effect.empty() ? "" : ", ") + mappingLabel(m) + " off";
      continue;
    }
    if (gestureActive_ && !m.firedThisGesture && m.trigger.matches(r)) {
      // An already-active note (e.g. a missed release) is retriggered.
      noteOff(m);
      noteOn(m);
      m.firedThisGesture = true;
      if (m.releaseMode == ReleaseMode::Timed) m.offAt = (nowMs + m.holdMs) | 1;
      effect += (effect.empty() ? "" : ", ") + mappingLabel(m) + " on";
    }
  }
  if (idle) endGesture();
  if (reportSeen) reportSeen(r, effect);
}

void Engine::tick(uint32_t nowMs) {
  if (learn_.active) {
    learnTick(nowMs);
    return;
  }
  if (gestureActive_ && nowMs - lastReportMs_ >= settings.gestureGapMs) endGesture();
  for (auto& m : mappings_) {
    if (m.active && m.offAt && static_cast<int32_t>(nowMs - m.offAt) >= 0) noteOff(m);
  }
}

// ---------------------------------------------------------------------------
// Learning

bool Engine::startLearn(uint32_t id) {
  Mapping* m = find(id);
  if (!m) return false;
  allNotesOff();
  gestureActive_ = false;
  learn_ = LearnState();
  learn_.active = true;
  learn_.mappingId = id;
  changed(false);
  logf("learn", "Learning " + mappingLabel(*m) + ": press the ring button " +
                    std::to_string(settings.learnSamples) + " times");
  return true;
}

void Engine::cancelLearn() {
  if (!learn_.active) return;
  learn_.active = false;
  learn_.capturing = false;
  changed(false);
  logf("learn", "Learning cancelled");
}

void Engine::learnReport(const HidReport& r, uint32_t nowMs) {
  if (!learn_.capturing) {
    if (r.isIdle()) {
      // Release of a key held longer than learnGapMs: belongs to the
      // previous sample.
      if (!learn_.samples.empty() && learn_.tailUntil &&
          static_cast<int32_t>(nowMs - learn_.tailUntil) < 0) {
        learn_.samples.back().push_back(r);
        learn_.tailUntil = 0;
        changed(false);
      }
      return;
    }
    if (static_cast<int>(learn_.samples.size()) >= settings.learnSamples) return;
    learn_.capturing = true;
    learn_.current.clear();
    learn_.tailUntil = 0;
  }
  if (learn_.current.size() < kMaxSampleReports) learn_.current.push_back(r);
  learn_.lastMs = nowMs;
  changed(false);
}

void Engine::learnTick(uint32_t nowMs) {
  if (learn_.capturing && nowMs - learn_.lastMs >= settings.learnGapMs) {
    learn_.capturing = false;
    learn_.samples.push_back(std::move(learn_.current));
    learn_.current.clear();
    const auto& s = learn_.samples.back();
    learn_.tailUntil = s.back().isIdle() ? 0 : (learn_.lastMs + settings.learnTailMs) | 1;
    changed(false);
    logf("learn", "Captured press " + std::to_string(learn_.samples.size()) + "/" +
                      std::to_string(settings.learnSamples) + " (" + std::to_string(s.size()) +
                      " reports)");
  }
  bool tailOpen = learn_.tailUntil && static_cast<int32_t>(nowMs - learn_.tailUntil) < 0;
  if (!learn_.capturing && !tailOpen &&
      static_cast<int>(learn_.samples.size()) >= settings.learnSamples) {
    finishLearn();
  }
}

void Engine::deriveFromSamples(Mapping& m, const std::vector<std::vector<HidReport>>& samples) {
  m.gesture.clear();
  m.release = Pattern();
  if (samples.empty()) return;

  size_t n = SIZE_MAX;
  for (const auto& s : samples) n = std::min(n, s.size());
  n = std::min(n, kMaxGestureLen);
  for (size_t i = 0; i < n; i++) {
    std::vector<const HidReport*> col;
    for (const auto& s : samples) col.push_back(&s[i]);
    m.gesture.push_back(Pattern::consensus(col));
  }

  // Release = the final report, if it is distinctive.
  m.releaseMode = ReleaseMode::GestureEnd;
  bool multi = std::all_of(samples.begin(), samples.end(), [](const std::vector<HidReport>& s) { return s.size() >= 2; });
  if (multi) {
    std::vector<const HidReport*> lasts;
    for (const auto& s : samples) lasts.push_back(&s.back());
    Pattern rel = Pattern::consensus(lasts);
    bool ok = rel.fixedBytes() > 0;
    for (const auto& s : samples) {
      for (size_t j = 0; ok && j + 1 < s.size(); j++) {
        if (rel.matches(s[j])) ok = false;  // would end the note mid-gesture
      }
    }
    if (ok) {
      m.release = rel;
      m.releaseMode = ReleaseMode::Report;
    }
  }

  m.triggerAuto = true;
  m.triggerIndex = -1;
  m.trigger = Pattern();
}

bool Engine::chooseTrigger(Mapping& m, const std::vector<Mapping>& others, std::string* conflictName) {
  int fallback = -1;
  std::string firstConflict;
  for (size_t i = 0; i < m.gesture.size(); i++) {
    const Pattern& p = m.gesture[i];
    if (p.fixedBytes() == 0 || p.matchesIdle()) continue;
    if (m.releaseMode == ReleaseMode::Report && p.overlaps(m.release)) continue;
    if (fallback < 0) fallback = static_cast<int>(i);

    const Mapping* clash = nullptr;
    for (const auto& o : others) {
      if (o.id == m.id) continue;
      for (const auto& q : o.gesture) {
        if (p.overlaps(q)) clash = &o;
      }
      if (o.release.overlaps(p) || o.trigger.overlaps(p)) clash = &o;
      if (clash) break;
    }
    if (!clash) {
      m.triggerIndex = static_cast<int>(i);
      m.trigger = p;
      return true;
    }
    if (firstConflict.empty()) firstConflict = mappingLabel(*clash);
  }
  if (fallback < 0 && !m.gesture.empty()) fallback = 0;
  m.triggerIndex = fallback;
  m.trigger = fallback >= 0 ? m.gesture[fallback] : Pattern();
  if (conflictName) *conflictName = firstConflict;
  return firstConflict.empty();
}

void Engine::finishLearn() {
  if (!learn_.active) return;
  if (learn_.capturing && !learn_.current.empty()) {
    learn_.samples.push_back(std::move(learn_.current));
    learn_.current.clear();
  }
  learn_.active = false;
  learn_.capturing = false;
  learn_.resultMappingId = learn_.mappingId;
  learn_.notes.clear();

  Mapping* m = find(learn_.mappingId);
  if (!m || learn_.samples.empty()) {
    learn_.notes.push_back("Nothing was captured; mapping unchanged.");
    logf("warn", learn_.notes.back());
    changed(false);
    return;
  }

  deriveFromSamples(*m, learn_.samples);
  std::string clash;
  if (!chooseTrigger(*m, othersThan(m->id), &clash)) {
    learn_.notes.push_back("Could not tell this button apart from " + clash + "; both will fire.");
  }
  if (m->trigger.empty()) {
    learn_.notes.push_back("No usable trigger report found (all reports were blank).");
  }

  // Other mappings whose trigger also shows up in this gesture would fire on
  // this button too. Move them to a later, unambiguous report if they can.
  for (auto& o : mappings_) {
    if (o.id == m->id || o.trigger.empty()) continue;
    bool hit = false;
    for (const auto& p : m->gesture) hit = hit || o.trigger.overlaps(p);
    if (!hit) continue;
    if (o.triggerAuto && !o.gesture.empty()) {
      int before = o.triggerIndex;
      std::string c2;
      bool clean = chooseTrigger(o, othersThan(o.id), &c2);
      if (o.triggerIndex != before) {
        learn_.notes.push_back("Moved " + mappingLabel(o) + "'s trigger to report #" +
                               std::to_string(o.triggerIndex) + " so the two buttons stay distinct.");
      }
      if (!clean) learn_.notes.push_back(mappingLabel(o) + " also fires on this button.");
    } else {
      learn_.notes.push_back(mappingLabel(o) + " (hand-edited trigger) also fires on this button.");
    }
  }

  std::string summary = "Learned " + mappingLabel(*m) + " from " + std::to_string(learn_.samples.size()) +
                        " presses: trigger report #" + std::to_string(m->triggerIndex) + ", release by " +
                        releaseModeName(m->releaseMode);
  learn_.notes.insert(learn_.notes.begin(), summary);
  for (size_t i = 0; i < learn_.notes.size(); i++) logf(i ? "warn" : "learn", learn_.notes[i]);
  changed(true);
}
